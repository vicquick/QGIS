/***************************************************************************
                         qgslibredwgreader.cpp
                         ---------------------
    begin                : June 2026
    copyright            : (C) 2026 by Victor Budinich

    qgis-ch issue #21, WP1 Option 2 — GNU LibreDWG DWG backend.

    Entity coverage verified by compile + symbol check against GNU LibreDWG
    0.13.4.8295 (latest, 2026-06-15). Covers every entity type found in
    production Vectorworks DWG exports: LINE, POLYLINE_2D (+VERTEX_2D),
    LWPOLYLINE, SPLINE, HATCH, SOLID, MTEXT, INSERT, plus the common set
    POINT/CIRCLE/ARC/ELLIPSE/TEXT and the LAYER table.
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgslibredwgreader.h"
#include "qgslogger.h"
#include "qgsmessagelog.h"

#include "drw_entities.h"
#include "drw_objects.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <vector>

// GNU LibreDWG public API (>= 0.13.4). Headers ship in libredwg-dev / our build.
extern "C"
{
#include <dwg.h>
#include <dwg_api.h>
// bit_convert_TU (UTF-16 -> UTF-8, malloc'd) is exported from libredwg but not in
// the public headers; declare it for block-name conversion on R2007+ files.
char *bit_convert_TU( const BITCODE_TU restrict_str );
}

namespace
{
  // Every BITCODE_T field (entity text, table entry names, hatch pattern names)
  // is decoded by bit_read_T, which returns UTF-16 on R2007+ and the drawing's
  // 8-bit code page before that — libredwg's IS_FROM_TU(dat) is
  // "dat->from_version >= R_2007 && !(dat->opts & DWG_OPTS_IN)" (bits.h:89), and
  // we never set DWG_OPTS_IN. The type stays char* either way, so a plain
  // std::string( ptr ) silently truncates the UTF-16 form at the first embedded
  // NUL: "Bestand" arrives as "B", and any surviving bytes are half-characters
  // that are not valid UTF-8 for the GeoPackage writer.
  //
  // bit_convert_TU returns a malloc'd UTF-8 copy that we own; the 8-bit form is
  // a borrowed pointer into Dwg_Data and must never be freed.
  std::string toUtf8( char *s, bool isTU )
  {
    if ( !s )
      return std::string();
    if ( !isTU )
      return std::string( s );
    char *u8 = bit_convert_TU( reinterpret_cast<BITCODE_TU>( s ) );
    std::string r = u8 ? std::string( u8 ) : std::string();
    std::free( u8 );
    return r;
  }

  // BITCODE_3BD / BITCODE_3DPOINT are {x,y,z}; BITCODE_2RD / BITCODE_2DPOINT are {x,y}.
  template <typename P> DRW_Coord toCoord( const P &p ) { DRW_Coord c; c.x = p.x; c.y = p.y; c.z = p.z; return c; }
  template <typename P> DRW_Coord toCoord2( const P &p ) { DRW_Coord c; c.x = p.x; c.y = p.y; c.z = 0; return c; }

  // Resolve a handle to a LAYER or LTYPE table entry and return its name.
  //
  // Deliberately avoids libredwg's name getters, whose ownership contract cannot
  // be satisfied by any single caller:
  //  * dwg_obj_table_get_name() returns a malloc'd copy when IS_FROM_TU_DWG, but
  //    a *borrowed* pointer straight into Dwg_Data otherwise (dwg_api.c:23408).
  //  * dwg_ent_get_layer_name()/dwg_ent_get_ltype_name() pass that through, and
  //    substitute the string literals "0"/"ByLayer" when the handle is unset.
  //    Three different ownerships behind one char*: freeing crashes on two of
  //    them, not freeing leaks the third.
  //  * dwg_obj_layer_get_name() gates its UTF-16 conversion on dwg_api.c's file
  //    static `dwg_version`, which is only ever assigned by
  //    dwg_api_init_version() (compiled out unless USE_DEPRECATED_API) and
  //    dwg_get_block_header(). This reader calls neither, so it stays R_INVALID
  //    (== 0, first in the enum) and the conversion never runs.
  //
  // Reading the field and converting it ourselves gives one owned std::string.
  std::string tableName( Dwg_Data *dwg, BITCODE_H ref, Dwg_Object_Type type, bool isTU )
  {
    if ( !dwg || !ref )
      return std::string();
    Dwg_Object *o = dwg_ref_object( dwg, ref );
    if ( !o || o->supertype != DWG_SUPERTYPE_OBJECT || o->fixedtype != type )
      return std::string();
    if ( type == DWG_TYPE_LAYER )
    {
      Dwg_Object_LAYER *l = dwg_object_to_LAYER( o );
      return l ? toUtf8( l->name, isTU ) : std::string();
    }
    if ( type == DWG_TYPE_LTYPE )
    {
      Dwg_Object_LTYPE *l = dwg_object_to_LTYPE( o );
      return l ? toUtf8( l->name, isTU ) : std::string();
    }
    return std::string();
  }

  // Normalise a LibreDWG linetype name to what QgsDwgImporter::linetypeString
  // expects: "bylayer"/"byblock" are special-cased there, CONTINUOUS means solid
  // and maps to the empty string, and every other name is looked up in the
  // mLinetype map.
  //
  // That map is keyed by addLType() with name.toLower(), while linetypeString()
  // queries it with the name unchanged — so anything not already lowercase misses
  // and silently resolves to a solid line. Lower the whole name, not just the two
  // special cases. ASCII only and by hand: UTF-8 continuation bytes are >= 0x80
  // and std::tolower on them is locale-dependent, which could corrupt a name.
  std::string normLineType( const std::string &lt )
  {
    std::string s( lt );
    for ( char &c : s )
      if ( c >= 'A' && c <= 'Z' )
        c = static_cast<char>( c - 'A' + 'a' );
    if ( s == "continuous" )
      return std::string();
    return s;
  }

  void fillCommon( DRW_Entity &e, Dwg_Object *obj, Dwg_Object_Entity *ent, bool isTU )
  {
    int err = 0;
    Dwg_Data *dwg = obj->parent;
    // Entity layer. DRW_Entity::layer already defaults to "0", which is the same
    // fallback dwg_ent_get_layer_name() applies when the handle does not resolve.
    const std::string layer = tableName( dwg, ent->layer, DWG_TYPE_LAYER, isTU );
    if ( !layer.empty() )
      e.layer = layer;
    if ( const Dwg_Color *col = dwg_ent_get_color( ent, &err ) )
    {
      e.color = col->index;          // ACI; 256 == BYLAYER, 0 == BYBLOCK
      // LibreDWG packs the colour method in the top byte of rgb: 0x02/0xC2 == true
      // RGB; 0xC0/0xC1 == ByLayer/ByBlock and must NOT be read as an explicit colour.
      const unsigned method = ( static_cast<unsigned>( col->rgb ) >> 24 ) & 0xffu;
      int color24 = ( method == 0x02u || method == 0xC2u ) ? static_cast<int>( col->rgb & 0xffffffu ) : -1;
      // Vectorworks/AutoCAD store most true colours in referenced DBCOLOR objects
      // (entity colour flag & 0x40). Resolve the colour handle -> DBCOLOR -> rgb. This
      // needs the libredwg common_entity_data colour-handle fix (defer handle read +
      // independent inline-RGB) so col->handle points at the right DBCOLOR.
      if ( color24 < 0 && ( col->flag & 0x40 ) && col->handle && dwg )
      {
        if ( Dwg_Object *dbo = dwg_ref_object( dwg, col->handle ) )
          if ( dbo->fixedtype == DWG_TYPE_DBCOLOR )
            color24 = static_cast<int>( dbo->tio.object->tio.DBCOLOR->color.rgb & 0xffffffu );
      }
      e.color24 = color24;
      // Entity transparency: colour flag & 0x20 carries a by-value alpha
      // (255 == opaque). DRW code 440 is stored as 255 - alpha, which
      // QgsDwgImporter::colorString() turns back into the rgba alpha. VW screens
      // its "Bestand" (existing-context) fills to ~60% (alpha 153) so the design
      // reads through them — without this they import fully opaque and obscure it.
      if ( col->flag & 0x20 )
        e.transparency = 0xff - static_cast<int>( col->alpha & 0xffu );
    }
    // Entity lineweight: LibreDWG's linewt byte uses the same index encoding as
    // DRW_LW_Conv::lineWidth (2 == 0.09mm, 7 == 0.25mm, 29 == ByLayer ...).
    e.lWeight = static_cast<DRW_LW_Conv::lineWidth>( static_cast<signed char>( ent->linewt ) );
    // Per-entity linetype scale (DXF 48). QgsDwgImporter::addEntity() writes it
    // to the "ltscale" column of every entity table; leaving it unset made every
    // entity claim DRW_Entity's 1.0 default no matter what the drawing said.
    e.ltypeScale = ent->ltype_scale;
    // Entity linetype. dwg_ent_get_ltype_name() does not resolve this: it reads
    // only ent->ltype, which common_entity_handle_data.spec decodes solely when
    // ltype_flags == 3 (0 ByLayer, 1 ByBlock, 2 Continuous, 3 explicit handle).
    // For flags 1 and 2 the handle is absent, so it returned its "ByLayer"
    // literal and ByBlock/Continuous entities were mislabelled — and because it
    // never writes through its error pointer, the `if ( !err )` guard that used
    // to wrap it always passed. R13/R14 have no ltype_flags at all (isbylayerlt
    // gates the handle there), so try the explicit handle first, then the flag.
    const std::string ltype = tableName( dwg, ent->ltype, DWG_TYPE_LTYPE, isTU );
    if ( !ltype.empty() )
      e.lineType = normLineType( ltype );
    else if ( ent->ltype_flags == 1 )
      e.lineType = std::string( "byblock" );
    else if ( ent->ltype_flags == 2 )
      e.lineType = std::string(); // CONTINUOUS == solid
    else
      e.lineType = std::string( "bylayer" );
    e.handle = static_cast<duint32>( obj->handle.value );
  }

  // TEXT, ATTRIB and ATTDEF all decode AcDbText, and libredwg spells its fields
  // identically on all three (dwg.h: Dwg_Entity_TEXT, Dwg_Entity_ATTRIB and
  // Dwg_Entity_ATTDEF each carry elevation, ins_pt, alignment_pt, thickness,
  // oblique_angle, rotation, height, width_factor, generation, horiz_alignment
  // and vert_alignment). One template therefore fills the DRW_Text that all
  // three are replayed as; only the string field differs, so the caller sets it.
  template <typename T> void fillText( DRW_Text &e, const T *o )
  {
    e.basePoint.x = o->ins_pt.x;
    e.basePoint.y = o->ins_pt.y;
    e.basePoint.z = o->elevation;
    // QgsDwgImporter::addText() places the feature at secPoint rather than
    // basePoint as soon as either alignment is non-zero, so centred, right
    // aligned and fitted text has to carry alignment_pt. DWG only stores that
    // point when an alignment is set — dwg.spec reads it under
    // "!(dataflags & 0x02)", and 0x02 is set exactly when codes 72 and 73 are
    // both zero — so when it is absent the aligns are zero and it is unread.
    e.secPoint.x = o->alignment_pt.x;
    e.secPoint.y = o->alignment_pt.y;
    e.secPoint.z = o->elevation;
    e.thickness = o->thickness;
    e.height = o->height;
    // Radians: libredwg keeps every angle in radians and only converts on DXF
    // output (out_dxf.c, group codes 50-54), and addText() does its own
    // `data.angle * 180.0 / M_PI`.
    e.angle = o->rotation;
    e.oblique = o->oblique_angle;
    // Always valid: R13/R14 read it unconditionally, and the R2000+ decoder
    // substitutes 1.0 when the drawing omits it (dwg.spec, dataflags & 0x10).
    e.widthscale = o->width_factor;
    e.textgen = o->generation;
    e.alignH = static_cast<DRW_Text::HAlign>( o->horiz_alignment );
    e.alignV = static_cast<DRW_Text::VAlign>( o->vert_alignment );
  }

  void addLwplBoundary( DRW_HatchLoop *loop, Dwg_HATCH_Path *path )
  {
    auto pl = std::make_shared<DRW_LWPolyline>();
    pl->flags = path->closed ? 1 : 0;
    for ( BITCODE_BL i = 0; path->polyline_paths && i < path->num_segs_or_paths; ++i )
    {
      auto v = std::make_shared<DRW_Vertex2D>();
      v->x = path->polyline_paths[i].point.x;
      v->y = path->polyline_paths[i].point.y;
      if ( path->bulges_present )
        v->bulge = path->polyline_paths[i].bulge;
      pl->vertlist.push_back( v );
    }
    loop->objlist.push_back( pl );
  }

  // Elliptical arc boundary edge (HATCH path curve type 3), densified into a
  // DRW_LWPolyline.
  //
  // It is not passed through as a DRW_Ellipse because DRW_Ellipse derives from
  // DRW_Line (drw_entities.h) and the dynamic_cast chain in
  // QgsDwgImporter::addHatch() tests DRW_LWPolyline, DRW_Line, DRW_Arc,
  // DRW_Spline in that order: an ellipse is caught by the DRW_Line branch and
  // silently collapsed into a straight chord from its centre to the end of its
  // major axis. That is what libdxfrw's own DWG reader produces here, so
  // elliptical hatch boundaries have never been right in either backend; going
  // through a polyline is a deliberate divergence from it.
  //
  // Parametrisation (dwg.spec, HATCH curve_type 3, DXF 10/11/40/50/51/73):
  // `center` C, `endpoint` is the major half-axis vector A measured from C,
  // `minor_major_ratio` r scales the perpendicular half-axis, and the two angles
  // are curve parameters — radians here, because libredwg keeps every angle in
  // radians and only converts on DXF output.
  //
  //   P(t) = C + cos(t) * A + sin(t) * r * rot90(A),   rot90(A) = ( -A.y, A.x )
  void addEllipseBoundary( DRW_HatchLoop *loop, Dwg_HATCH_PathSeg *seg )
  {
    const double ax = seg->endpoint.x;
    const double ay = seg->endpoint.y;
    if ( ax == 0.0 && ay == 0.0 )
      return; // degenerate: no major axis to sweep
    const double bx = -ay * seg->minor_major_ratio;
    const double by = ax * seg->minor_major_ratio;

    // fmod keeps |sweep| < 2*pi with the sign of the difference, so one
    // adjustment is enough to turn it into a sweep in the requested direction —
    // and, unlike repeated addition, it cannot spin forever on a bad value.
    // A full ellipse is written as start == end, which lands on 0 and becomes a
    // whole turn.
    double sweep = std::fmod( seg->end_angle - seg->start_angle, 2 * M_PI );
    if ( !std::isfinite( sweep ) || !std::isfinite( seg->start_angle ) )
      return;
    if ( seg->is_ccw )
    {
      if ( sweep <= 0 )
        sweep += 2 * M_PI;
    }
    else
    {
      if ( sweep >= 0 )
        sweep -= 2 * M_PI;
    }

    // 64 chords per full turn; a boundary only has to bound the fill.
    int nseg = static_cast<int>( std::ceil( std::fabs( sweep ) / ( 2 * M_PI ) * 64.0 ) );
    if ( nseg < 2 )
      nseg = 2;

    auto pl = std::make_shared<DRW_LWPolyline>();
    for ( int k = 0; k <= nseg; ++k )
    {
      const double t = seg->start_angle + sweep * k / nseg;
      auto v = std::make_shared<DRW_Vertex2D>();
      v->x = seg->center.x + std::cos( t ) * ax + std::sin( t ) * bx;
      v->y = seg->center.y + std::cos( t ) * ay + std::sin( t ) * by;
      pl->vertlist.push_back( v );
    }
    loop->objlist.push_back( pl );
  }

  // Spline boundary edge (HATCH path curve type 4). QgsDwgImporter::addHatch()
  // already has a DRW_Spline branch in its dynamic_cast chain and runs it
  // through lineFromSpline(), so the edge only has to be handed over as one.
  //
  // This is newly worth doing: libredwg used to read the two spline tangents
  // unconditionally on R2010+, overran the object whenever the edge had no fit
  // points, and lost every remaining edge and path of the hatch. Since the
  // decoder only reads them under num_fitpts (dwg.spec, HATCH curve_type 4) the
  // rest of the boundary survives a spline edge.
  //
  // lineFromSpline() rejects degrees outside 1..3 and returns leaving its
  // QgsLineString empty, but addHatch() ignores its return value and adds the
  // empty curve to the ring anyway — so screen the degree here and fall back to
  // a polyline through the points we do have rather than hand over a spline it
  // cannot evaluate.
  void addSplineBoundary( DRW_HatchLoop *loop, Dwg_HATCH_PathSeg *seg )
  {
    const BITCODE_BL nc = seg->control_points ? seg->num_control_points : 0;
    const BITCODE_BL nf = seg->fitpts ? seg->num_fitpts : 0;

    // rbspline() needs at least degree+1 control points for an order-degree+1
    // basis; fewer and it would read past the end of its own control vector.
    if ( seg->degree >= 1 && seg->degree <= 3 && nc > seg->degree )
    {
      auto sp = std::make_shared<DRW_Spline>();
      sp->degree = static_cast<int>( seg->degree );
      // Same encoding libdxfrw's DWG reader gives this edge (drw_entities.cpp,
      // DRW_Hatch::parseDwg): bit 2 periodic, bit 4 rational. The closed bit (1)
      // is deliberately left clear — lineFromSpline() reads it as "wrap the
      // control polygon and evaluate a periodic basis", which is not what a
      // hatch boundary edge describes.
      sp->flags = ( seg->is_rational ? 4 : 0 ) | ( seg->is_periodic ? 2 : 0 );
      for ( BITCODE_BL k = 0; seg->knots && k < seg->num_knots; ++k )
        sp->knotslist.push_back( seg->knots[k] );
      for ( BITCODE_BL c = 0; c < nc; ++c )
      {
        sp->controllist.push_back( std::make_shared<DRW_Coord>( seg->control_points[c].point.x, seg->control_points[c].point.y, 0.0 ) );
        // Only decoded when the edge says it is rational; 0 otherwise.
        if ( seg->is_rational )
          sp->weightlist.push_back( seg->control_points[c].weight );
      }
      sp->ncontrol = static_cast<int>( nc );
      sp->nknots = static_cast<int>( seg->num_knots );
      loop->objlist.push_back( sp );
      return;
    }

    // Degenerate or higher-degree edge: approximate it so the ring still closes.
    // Fit points lie on the curve, control points only bound it, so prefer them.
    const bool useFit = nf >= 2;
    const BITCODE_BL n = useFit ? nf : nc;
    if ( n < 2 )
      return;
    auto pl = std::make_shared<DRW_LWPolyline>();
    for ( BITCODE_BL k = 0; k < n; ++k )
    {
      auto v = std::make_shared<DRW_Vertex2D>();
      v->x = useFit ? seg->fitpts[k].x : seg->control_points[k].point.x;
      v->y = useFit ? seg->fitpts[k].y : seg->control_points[k].point.y;
      pl->vertlist.push_back( v );
    }
    loop->objlist.push_back( pl );
  }

  // Dwg_Version_Type is ordered and carries every point/beta release between the
  // headline versions (R_13b1, R_13b2, R_13c3, R_2000b, R_2000i, R_2002,
  // R_2004a..c, R_2007a/b, R_2010b, R_2013b, R_2018b — dwg.h), while all
  // releases inside one format generation share a single AC10xx code. Matching
  // exact values therefore mapped R13/R14 and every point release to UNKNOWNV,
  // which QgsDwgImporter::import() renders as "unsupported version. Cannot read
  // <version> documents." Compare ranges instead.
  DRW::Version mapVersion( Dwg_Version_Type v )
  {
    if ( v >= R_2018b ) return DRW::AC1032;
    if ( v >= R_2013b ) return DRW::AC1027;
    if ( v >= R_2010b ) return DRW::AC1024;
    if ( v >= R_2007a ) return DRW::AC1021;
    if ( v >= R_2004a ) return DRW::AC1018;
    if ( v >= R_2000b ) return DRW::AC1015;
    if ( v >= R_14 )    return DRW::AC1014;
    if ( v >= R_13b1 )  return DRW::AC1012;
    return DRW::UNKNOWNV; // pre-R13: no DRW::Version code, and unsupported here
  }
}

QgsLibreDwgReader::QgsLibreDwgReader( const std::string &fileName )
  : mFileName( fileName )
{
}

QgsLibreDwgReader::~QgsLibreDwgReader() = default;

bool QgsLibreDwgReader::read( DRW_Interface *iface, bool /*expandInserts*/ )
{
  Dwg_Data dwg;
  memset( &dwg, 0, sizeof( dwg ) );

  // dwg_read_file returns a bitmask of DWG_ERR_*. Anything below
  // DWG_ERR_CRITICAL (unsupported/unhandled classes, value-out-of-bounds) is
  // tolerable — we replay whatever entities parsed, which is the whole point:
  // skip the unreadable instead of crashing as libdxfrw's decompress18 does.
  const int rc = dwg_read_file( mFileName.c_str(), &dwg );
  if ( rc >= DWG_ERR_CRITICAL )
  {
    QgsDebugError( QStringLiteral( "LibreDWG dwg_read_file failed on %1 (rc=0x%2)" )
                     .arg( QString::fromStdString( mFileName ) ).arg( rc, 0, 16 ) );
    mError = ( rc & DWG_ERR_INVALIDDWG ) ? DRW::BAD_READ_FILE_HEADER : DRW::BAD_OPEN;
    dwg_free( &dwg );
    return false;
  }

  mVersion = mapVersion( dwg.header.version );

  // Whether this drawing's BITCODE_T strings are UTF-16 — see toUtf8().
  const bool isTU = dwg.header.from_version >= R_2007;

  // First pass: symbol tables. LTYPE has to be replayed before LAYER —
  // QgsDwgImporter::addLayer() resolves the layer's linetype name against the
  // mLinetype map right away and caches the result in mLayerLinetype, so a LAYER
  // that arrives before its LTYPE caches an empty (solid) dash pattern for good.
  // Object order in the file does not guarantee that, hence two loops.
  for ( BITCODE_BL i = 0; i < dwg.num_objects; ++i )
  {
    Dwg_Object *obj = &dwg.object[i];
    if ( obj->supertype != DWG_SUPERTYPE_OBJECT || obj->fixedtype != DWG_TYPE_LTYPE )
      continue;
    Dwg_Object_LTYPE *lt = dwg_object_to_LTYPE( obj );
    if ( !lt )
      continue;
    DRW_LType dl;
    dl.name = toUtf8( lt->name, isTU );
    if ( lt->dashes )
      for ( BITCODE_RC d = 0; d < lt->numdashes; ++d )
        dl.path.push_back( lt->dashes[d].length );
    iface->addLType( dl );
  }

  for ( BITCODE_BL i = 0; i < dwg.num_objects; ++i )
  {
    Dwg_Object *obj = &dwg.object[i];
    if ( obj->supertype != DWG_SUPERTYPE_OBJECT || obj->fixedtype != DWG_TYPE_LAYER )
      continue;
    Dwg_Object_LAYER *lay = dwg_object_to_LAYER( obj );
    if ( !lay )
      continue;
    DRW_Layer dl;
    // DRW_Layer::transparency (drw_objects.h) is a bare int that neither the
    // constructor nor reset() ever assigns, and QgsDwgImporter::addLayer() feeds
    // it straight into colorString() as `255 - ( transparency & 0xff )` and into
    // the layers table. Reading it uninitialised gave the layer colour a random
    // alpha. Dwg_Object_LAYER carries no transparency field at all (dwg.h), so
    // opaque is both the correct value and the only one available.
    dl.transparency = DRW::Opaque;
    dl.name = toUtf8( lay->name, isTU );
    dl.color = lay->color.index;
    const unsigned m = ( static_cast<unsigned>( lay->color.rgb ) >> 24 ) & 0xffu;
    dl.color24 = ( m == 0x02u || m == 0xC2u ) ? static_cast<int>( lay->color.rgb & 0xffffffu ) : -1;
    dl.lWeight = static_cast<DRW_LW_Conv::lineWidth>( static_cast<signed char>( lay->linewt ) );
    // Layer linetype (DXF code 6). Never set before, so addLayer() kept
    // DRW_Layer's "CONTINUOUS" default and cached a solid pattern for the layer —
    // which is what every BYLAYER entity in the drawing then resolved to.
    dl.lineType = normLineType( tableName( &dwg, lay->ltype, DWG_TYPE_LTYPE, isTU ) );
    iface->addLayer( dl );
  }

  // Second pass: entities. Each one is self-contained — a POLYLINE_2D pulls its
  // own VERTEX_2D subentities, so no state carries between callbacks.
  //
  // Entities are emitted block-by-block so QgsDwgImporter::expandInserts() can
  // place INSERTs: each non-layout BLOCK_HEADER is wrapped in addBlock/endBlock
  // (its geometry tagged with the block), model-space geometry is emitted
  // directly, and INSERTs carry their block name + transform. Without this the
  // block contents (Vectorworks emits almost everything inside "Gruppe-*"
  // blocks) land at block-local coords instead of each insert location.
  int emitted = 0;

  // Block names must match exactly between addBlock and addInsert (expandInserts
  // pairs them by name), so both go through the same conversion.
  auto blockName = [&]( Dwg_Object_BLOCK_HEADER *bh ) -> std::string {
    return bh ? toUtf8( bh->name, isTU ) : std::string();
  };

  auto emitEntity = [&]( Dwg_Object *obj ) {
    // obj->parent is dereferenced by fillCommon() and by
    // get_first_owned_subentity(), so it has to be part of the entry check.
    if ( !obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity || !obj->parent )
      return;
    Dwg_Object_Entity *ent = obj->tio.entity;

    switch ( obj->fixedtype )
    {
      case DWG_TYPE_POINT:
      {
        Dwg_Entity_POINT *o = dwg_object_to_POINT( obj );
        if ( !o )
          break;
        DRW_Point e; fillCommon( e, obj, ent, isTU );
        e.basePoint.x = o->x; e.basePoint.y = o->y; e.basePoint.z = o->z;
        iface->addPoint( e ); ++emitted; break;
      }
      case DWG_TYPE_LINE:
      {
        Dwg_Entity_LINE *o = dwg_object_to_LINE( obj );
        if ( !o )
          break;
        DRW_Line e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->start ); e.secPoint = toCoord( o->end );
        iface->addLine( e ); ++emitted; break;
      }
      case DWG_TYPE_CIRCLE:
      {
        Dwg_Entity_CIRCLE *o = dwg_object_to_CIRCLE( obj );
        if ( !o )
          break;
        DRW_Circle e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->center ); e.radius = o->radius;
        iface->addCircle( e ); ++emitted; break;
      }
      case DWG_TYPE_ARC:
      {
        Dwg_Entity_ARC *o = dwg_object_to_ARC( obj );
        if ( !o )
          break;
        DRW_Arc e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->center ); e.radius = o->radius;
        e.staangle = o->start_angle; e.endangle = o->end_angle;
        iface->addArc( e ); ++emitted; break;
      }
      case DWG_TYPE_ELLIPSE:
      {
        Dwg_Entity_ELLIPSE *o = dwg_object_to_ELLIPSE( obj );
        if ( !o )
          break;
        DRW_Ellipse e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->center ); e.secPoint = toCoord( o->sm_axis );
        e.ratio = o->axis_ratio; e.staparam = o->start_angle; e.endparam = o->end_angle;
        iface->addEllipse( e ); ++emitted; break;
      }
      case DWG_TYPE_LWPOLYLINE:
      {
        Dwg_Entity_LWPOLYLINE *o = dwg_object_to_LWPOLYLINE( obj );
        if ( !o )
          break;
        DRW_LWPolyline e; fillCommon( e, obj, ent, isTU );
        e.flags = ( o->flag & 512 ) ? 1 : 0;
        e.elevation = o->elevation;
        for ( BITCODE_BL v = 0; o->points && v < o->num_points; ++v )
        {
          auto vx = std::make_shared<DRW_Vertex2D>();
          vx->x = o->points[v].x; vx->y = o->points[v].y;
          if ( o->bulges && v < o->num_bulges )
            vx->bulge = o->bulges[v];
          e.vertlist.push_back( vx );
        }
        iface->addLWPolyline( e ); ++emitted; break;
      }
      case DWG_TYPE_POLYLINE_2D:
      {
        Dwg_Entity_POLYLINE_2D *o = dwg_object_to_POLYLINE_2D( obj );
        if ( !o )
          break;
        DRW_Polyline e; fillCommon( e, obj, ent, isTU );
        e.flags = o->flag;
        e.basePoint.z = o->elevation;
        // VERTEX_2D and SEQEND are subentities, so they never reach this switch:
        // they are absent from the block's entities[] vector on R2004+, and
        // get_next_owned_entity() skips them explicitly on R13-R2000. Read them
        // off the polyline itself — vertex[] on R2004+, the
        // first_vertex..last_vertex chain before that, which is exactly what
        // get_first_owned_subentity()/get_next_owned_subentity() abstract over.
        //
        // dwg_object_polyline_2d_get_points() is not usable here: dwg_point_2d
        // has no bulge, and the function returns NULL as soon as its walk sets
        // *error, which the R13-R2000 branch does for any non-VERTEX_2D it meets
        // in the chain.
        //
        // get_first_owned_subentity() does not reset the shared iterator on
        // R2004+ (only running to completion does), so clear it ourselves, and
        // bound the walk the same way the block walk is bounded.
        ent->__iterator = 0;
        BITCODE_BL steps = 0;
        for ( Dwg_Object *vo = get_first_owned_subentity( obj ); vo; vo = get_next_owned_subentity( obj, vo ) )
        {
          if ( ++steps > dwg.num_objects )
            break;
          if ( vo->fixedtype != DWG_TYPE_VERTEX_2D )
          {
            // A polyline's vertex run is contiguous and closed by SEQEND, so
            // anything after the first vertex means the chain has left this
            // polyline. Stopping there also keeps the R13-R2000 branch of
            // get_next_owned_subentity() from scanning the rest of the object
            // array whenever last_vertex fails to resolve.
            if ( !e.vertlist.empty() )
              break;
            continue;
          }
          Dwg_Entity_VERTEX_2D *vd = dwg_object_to_VERTEX_2D( vo );
          if ( !vd )
            continue;
          auto v = std::make_shared<DRW_Vertex>();
          v->basePoint = toCoord( vd->point );
          v->bulge = vd->bulge;
          e.appendVertex( v );
        }
        if ( e.vertlist.empty() ) // addPolyline() rejects these anyway
          break;
        iface->addPolyline( e ); ++emitted; break;
      }
      case DWG_TYPE_SPLINE:
      {
        Dwg_Entity_SPLINE *o = dwg_object_to_SPLINE( obj );
        if ( !o )
          break;
        DRW_Spline e; fillCommon( e, obj, ent, isTU );
        e.degree = o->degree;
        e.flags = o->flag;
        e.tgStart = toCoord( o->beg_tan_vec );
        e.tgEnd = toCoord( o->end_tan_vec );
        for ( BITCODE_BL k = 0; o->knots && k < o->num_knots; ++k )
          e.knotslist.push_back( o->knots[k] );
        for ( BITCODE_BL c = 0; o->ctrl_pts && c < o->num_ctrl_pts; ++c )
        {
          e.controllist.push_back( std::make_shared<DRW_Coord>( o->ctrl_pts[c].x, o->ctrl_pts[c].y, o->ctrl_pts[c].z ) );
          if ( o->weighted )
            e.weightlist.push_back( o->ctrl_pts[c].w );
        }
        e.ncontrol = static_cast<int>( o->num_ctrl_pts );
        e.nknots = static_cast<int>( o->num_knots );
        iface->addSpline( &e ); ++emitted; break;
      }
      case DWG_TYPE_SOLID:
      {
        Dwg_Entity_SOLID *o = dwg_object_to_SOLID( obj );
        if ( !o )
          break;
        DRW_Solid e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord2( o->corner1 );
        e.secPoint = toCoord2( o->corner2 );
        e.thirdPoint = toCoord2( o->corner3 );
        e.forthPoint = toCoord2( o->corner4 );
        e.basePoint.z = o->elevation;
        iface->addSolid( e ); ++emitted; break;
      }
      case DWG_TYPE_TEXT:
      {
        Dwg_Entity_TEXT *o = dwg_object_to_TEXT( obj );
        if ( !o )
          break;
        DRW_Text e; fillCommon( e, obj, ent, isTU );
        fillText( e, o );
        e.text = toUtf8( o->text_value, isTU );
        iface->addText( e ); ++emitted; break;
      }
      case DWG_TYPE_MTEXT:
      {
        Dwg_Entity_MTEXT *o = dwg_object_to_MTEXT( obj );
        if ( !o )
          break;
        DRW_MText e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->ins_pt );
        e.height = o->text_height;
        e.text = toUtf8( o->text, isTU );
        iface->addMText( e ); ++emitted; break;
      }
      case DWG_TYPE_INSERT:
      {
        Dwg_Entity_INSERT *o = dwg_object_to_INSERT( obj );
        if ( !o )
          break;
        DRW_Insert e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->ins_pt );
        e.xscale = o->scale.x; e.yscale = o->scale.y; e.zscale = o->scale.z;
        e.angle = o->rotation;
        // Resolve the block name via the referenced BLOCK_HEADER (o->block_name
        // is often empty / raw TU). Must match the addBlock name so
        // expandInserts can pair the insert with its block definition.
        if ( o->block_header )
        {
          if ( Dwg_Object *bo = dwg_ref_object( &dwg, o->block_header ) )
            if ( bo->fixedtype == DWG_TYPE_BLOCK_HEADER && bo->tio.object )
              e.name = blockName( bo->tio.object->tio.BLOCK_HEADER );
        }
        iface->addInsert( e ); ++emitted; break;
      }
      case DWG_TYPE_HATCH:
      {
        Dwg_Entity_HATCH *o = dwg_object_to_HATCH( obj );
        if ( !o )
          break;
        DRW_Hatch e; fillCommon( e, obj, ent, isTU );
        e.solid = o->is_solid_fill;
        e.associative = o->is_associative;
        e.angle = o->angle;
        e.scale = o->scale_spacing;
        // Pattern metadata. The "hatches" table has had hstyle/hpattern/
        // doubleflag/deflines columns all along (qgsdwgimporter.cpp, hatches
        // table definition) and libdxfrw's DWG reader fills all four
        // (drw_entities.cpp, DRW_Hatch::parseDwg), so leaving them at the
        // DRW_Hatch defaults made this backend disagree with the incumbent one:
        // every hatch claimed style 0 (normal), pattern type 1 (predefined),
        // single-direction and zero definition lines.
        //
        // Names differ between the two libraries but the fields are the same
        // DXF codes: style == 75, pattern_type == 76, double_flag == 77,
        // num_deflines == 78 (dwg.spec, HATCH). libredwg only decodes angle,
        // scale_spacing, double_flag and num_deflines for pattern fills; for a
        // solid fill they stay 0, which is also what AutoCAD writes.
        e.hstyle = o->style;
        e.hpattern = o->pattern_type;
        e.doubleflag = o->double_flag;
        e.deflines = o->num_deflines;
        // HATCH::name is decoded with FIELD_T (dwg.spec), so it is TU on R2007+
        // like every other BITCODE_T, despite being declared BITCODE_TV.
        e.name = toUtf8( o->name, isTU );
        e.loopsnum = o->num_paths;
        for ( BITCODE_BL p = 0; o->paths && p < o->num_paths; ++p )
        {
          Dwg_HATCH_Path *path = &o->paths[p];
          auto loop = std::make_shared<DRW_HatchLoop>( static_cast<int>( path->flag ) );
          if ( path->flag & 2 ) // polyline boundary
          {
            addLwplBoundary( loop.get(), path );
          }
          else
          {
            for ( BITCODE_BL s = 0; path->segs && s < path->num_segs_or_paths; ++s )
            {
              Dwg_HATCH_PathSeg *seg = &path->segs[s];
              if ( seg->curve_type == 1 ) // line
              {
                auto ln = std::make_shared<DRW_Line>();
                ln->basePoint = toCoord2( seg->first_endpoint );
                ln->secPoint = toCoord2( seg->second_endpoint );
                loop->objlist.push_back( ln );
              }
              else if ( seg->curve_type == 2 ) // circular arc
              {
                auto ar = std::make_shared<DRW_Arc>();
                ar->basePoint = toCoord2( seg->center );
                ar->radius = seg->radius;
                ar->staangle = seg->start_angle; ar->endangle = seg->end_angle;
                loop->objlist.push_back( ar );
              }
              else if ( seg->curve_type == 3 ) // elliptical arc
              {
                addEllipseBoundary( loop.get(), seg );
              }
              else if ( seg->curve_type == 4 ) // spline
              {
                addSplineBoundary( loop.get(), seg );
              }
            }
          }
          e.looplist.push_back( loop );
        }
        iface->addHatch( &e ); ++emitted; break;
      }

      // Skipped (rare / non-vector): WIPEOUT (image mask), VIEWPORT (paper
      // space), 3DFACE, TRACE, RAY, XLINE, DIMENSION_*, LEADER, MLINE, IMAGE.
      default:
        break;
    }
  };

  // Cycle guard for the linked-list walk below. Stamping each visited object is
  // exact and resets in O(1) between blocks, unlike a plain step budget.
  std::vector<BITCODE_BL> visitStamp( dwg.num_objects, 0 );
  BITCODE_BL walkId = 0;

  // Owner -> entities index, built once and only if needed. Rescues drawings
  // whose owned-entity chain libredwg cannot walk at all: AC1016 and AC1017
  // decode to header.version R_2000i / R_2002, which fall outside *both*
  // branches of get_first_owned_entity() ("R_13b1 <= v <= R_2000" and
  // "v >= R_2004"), so it logs "Unsupported version" and returns NULL. dwg.spec
  // gates the BLOCK_HEADER handles by the same ranges, so first_entity and
  // entities[] are not even decoded for those files. The entities themselves
  // still record their owner: entmode 2 is model space, and entmode 0 carries an
  // explicit ownerhandle (dwg.h Dwg_Object_Entity::entmode,
  // common_entity_handle_data.spec:25).
  std::unordered_map<BITCODE_RLL, std::vector<Dwg_Object *>> ownedBy;
  std::vector<Dwg_Object *> mspaceOwned;
  bool ownerIndexBuilt = false;

  auto buildOwnerIndex = [&]() {
    if ( ownerIndexBuilt )
      return;
    ownerIndexBuilt = true;
    for ( BITCODE_BL i = 0; i < dwg.num_objects; ++i )
    {
      Dwg_Object *o = &dwg.object[i];
      if ( o->supertype != DWG_SUPERTYPE_ENTITY || !o->tio.entity )
        continue;
      // Subentities are owned by their POLYLINE/INSERT, never by the block, and
      // are replayed from there. Skip them explicitly rather than relying on
      // their entmode.
      switch ( o->fixedtype )
      {
        case DWG_TYPE_VERTEX_2D:
        case DWG_TYPE_VERTEX_3D:
        case DWG_TYPE_VERTEX_MESH:
        case DWG_TYPE_VERTEX_PFACE:
        case DWG_TYPE_VERTEX_PFACE_FACE:
        case DWG_TYPE_ATTRIB:
        case DWG_TYPE_ATTDEF:
        case DWG_TYPE_SEQEND:
          continue;
        default:
          break;
      }
      Dwg_Object_Entity *oe = o->tio.entity;
      if ( oe->ownerhandle && oe->ownerhandle->absolute_ref )
        ownedBy[oe->ownerhandle->absolute_ref].push_back( o );
      else if ( oe->entmode == 2 )
        mspaceOwned.push_back( o );
    }
  };

  auto blockHasOwned = [&]( Dwg_Object *hdrObj, Dwg_Object_BLOCK_HEADER *bh ) -> bool {
    if ( ( bh->entities && bh->num_owned ) || bh->first_entity )
      return true;
    buildOwnerIndex();
    return ownedBy.find( hdrObj->handle.value ) != ownedBy.end();
  };

  // Replay every entity owned by a BLOCK_HEADER, whichever way the file stores
  // the ownership. Returns how many were handed to emitEntity().
  auto emitOwned = [&]( Dwg_Object *hdrObj, bool modelSpace ) -> BITCODE_BL {
    if ( !hdrObj || hdrObj->supertype != DWG_SUPERTYPE_OBJECT || !hdrObj->tio.object
         || hdrObj->fixedtype != DWG_TYPE_BLOCK_HEADER || !hdrObj->parent )
      return 0; // get_first_owned_entity() dereferences all of these before it checks
    Dwg_Object_BLOCK_HEADER *bh = hdrObj->tio.object->tio.BLOCK_HEADER;
    if ( !bh )
      return 0;

    BITCODE_BL n = 0;
    if ( bh->entities && bh->num_owned )
    {
      // R2004+ (and pre-R13): the entities[] handle vector. Index it directly
      // instead of going through get_next_owned_entity(), which returns NULL at
      // the first unresolvable handle and would truncate the rest of the block.
      for ( BITCODE_BL k = 0; k < bh->num_owned; ++k )
        if ( Dwg_Object *e = dwg_ref_object( &dwg, bh->entities[k] ) )
        {
          emitEntity( e );
          ++n;
        }
    }
    else if ( bh->first_entity )
    {
      // R13-R2000: a first_entity/last_entity linked list. dwg.spec decodes
      // entities[] and num_owned only IF_FREE_OR_SINCE (R_2004a), so on these
      // files num_owned is 0 and entities is NULL — reading them yielded nothing
      // whatsoever, which is why R13/R14/R2000 drawings imported empty.
      //
      // Bound the walk ourselves: dwg_next_entity() rejects only a self-loop
      // (obj == next_obj) and otherwise falls through to a forward scan of the
      // whole object array, and the subentity-skipping loop inside
      // get_next_owned_entity() advances without bumping its own step counter —
      // so an A->B->A next_entity chain in a damaged file never terminates.
      // The claim is read-global, not per-walk. When last_entity->obj is NULL
      // on a damaged file, dwg_next_entity() abandons the linked list and
      // linear-scans dwg->object[] forward, handing back entities owned by
      // *other* block headers. A per-walk stamp cannot see that: those objects
      // are unstamped in this walk, so the block would swallow the rest of the
      // file, model space would be emitted twice, and expandInserts() would
      // then copy the over-stuffed block into every INSERT. That is worse than
      // the empty import this branch exists to fix. Any non-zero stamp means
      // some earlier header already emitted the object, so the walk stops.
      ++walkId;
      for ( Dwg_Object *e = get_first_owned_entity( hdrObj ); e; e = get_next_owned_entity( hdrObj, e ) )
      {
        if ( e->index >= dwg.num_objects || visitStamp[e->index] != 0 )
          break;
        visitStamp[e->index] = walkId;
        emitEntity( e );
        ++n;
      }
    }

    if ( n == 0 )
    {
      // Nothing came out of either representation — fall back on the owner index.
      // Only reachable when this header emitted nothing, so it cannot duplicate.
      buildOwnerIndex();
      const auto it = ownedBy.find( hdrObj->handle.value );
      if ( it != ownedBy.end() )
        for ( Dwg_Object *e : it->second )
        {
          emitEntity( e );
          ++n;
        }
      if ( modelSpace )
        for ( Dwg_Object *e : mspaceOwned )
        {
          emitEntity( e );
          ++n;
        }
    }
    return n;
  };

  Dwg_Object *msObj = dwg_model_space_object( &dwg );
  Dwg_Object *psObj = dwg_paper_space_object( &dwg );

  // Block definitions: every BLOCK_HEADER except the model/paper-space layouts.
  // Their geometry is tagged with the block (mBlockHandle) so expandInserts can
  // copy + transform it to each INSERT location.
  for ( BITCODE_BL i = 0; i < dwg.num_objects; ++i )
  {
    Dwg_Object *obj = &dwg.object[i];
    if ( obj->supertype != DWG_SUPERTYPE_OBJECT || obj->fixedtype != DWG_TYPE_BLOCK_HEADER )
      continue;
    if ( obj == msObj || obj == psObj )
      continue;
    Dwg_Object_BLOCK_HEADER *bh = obj->tio.object ? obj->tio.object->tio.BLOCK_HEADER : nullptr;
    if ( !bh )
      continue;
    // Externally referenced drawings: the geometry lives in another file, is not
    // in this file's object array, and must not be expanded into the INSERTs
    // that point at it.
    //
    // They were skipped only by accident. dwg.spec decodes
    // BLOCK_HEADER::num_owned on R2004+ only `if (!blkisxref && !xrefoverlaid)`,
    // so blockHasOwned() happened to find nothing to walk. That is a property of
    // the decoder, not a decision here, and it says nothing about R13-R2000
    // files, where first_entity is decoded either way. Test the flags.
    //
    // Either flag alone is ambiguous — a stale or hand-built header can carry
    // one with nothing behind it — so also require the path to the referenced
    // drawing. xref_pname is FIELD_T since R13 (dwg.spec, BLOCK_HEADER), hence
    // UTF-16 on R2007+ like every other BITCODE_T, so it goes through toUtf8()
    // rather than a raw NUL test.
    if ( ( bh->blkisxref || bh->xrefoverlaid ) && !toUtf8( bh->xref_pname, isTU ).empty() )
      continue;
    if ( !blockHasOwned( obj, bh ) )
      continue;
    DRW_Block db;
    db.name = blockName( bh );
    db.basePoint = toCoord( bh->base_pt );
    db.handle = static_cast<duint32>( obj->handle.value );
    iface->addBlock( db );
    emitOwned( obj, false );
    iface->endBlock();
  }

  // Model space: direct geometry plus the INSERT references that expandInserts
  // will resolve against the block definitions emitted above.
  emitOwned( msObj, true );

  QgsDebugMsgLevel( QStringLiteral( "LibreDWG: emitted %1 entities from %2 objects (version %3)" )
                      .arg( emitted ).arg( dwg.num_objects ).arg( static_cast<int>( dwg.header.version ) ), 2 );

  if ( emitted == 0 )
  {
    // A drawing that yields nothing is a reader problem far more often than it is
    // an empty file, and import() reports DRW::BAD_NONE ("No error.") whenever
    // read() returns true — so this used to be an empty GeoPackage plus a success
    // message. QgsDebugError and QgsDebugMsgLevel both compile to no-ops in
    // release builds, so route it to the log the importer itself writes to.
    QgsMessageLog::logMessage(
      QObject::tr( "No entities could be read from %1 (LibreDWG backend, %2 objects, version %3). The import will be empty." )
        .arg( QString::fromStdString( mFileName ) )
        .arg( dwg.num_objects )
        .arg( QString::fromUtf8( dwg_version_type( dwg.header.version ) ) ),
      QObject::tr( "DWG/DXF import" )
    );
  }

  dwg_free( &dwg );
  mError = DRW::BAD_NONE;
  return true;
}
