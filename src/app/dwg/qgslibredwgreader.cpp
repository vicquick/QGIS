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

#include "drw_entities.h"
#include "drw_objects.h"

#include <cmath>
#include <cstdlib>
#include <memory>

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
    free( u8 );
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

  void addLwplBoundary( DRW_HatchLoop *loop, Dwg_HATCH_Path *path )
  {
    auto pl = std::make_shared<DRW_LWPolyline>();
    pl->flags = path->closed ? 1 : 0;
    for ( BITCODE_BL i = 0; i < path->num_segs_or_paths; ++i )
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

  // Second pass: entities. POLYLINE_2D / VERTEX_2D / SEQEND are emitted as a
  // single DRW_Polyline accumulated across the contiguous object run, matching
  // how libdxfrw's dwgReader feeds QgsDwgImporter.
  //
  // Entities are emitted block-by-block so QgsDwgImporter::expandInserts() can
  // place INSERTs: each non-layout BLOCK_HEADER is wrapped in addBlock/endBlock
  // (its geometry tagged with the block), model-space geometry is emitted
  // directly, and INSERTs carry their block name + transform. Without this the
  // block contents (Vectorworks emits almost everything inside "Gruppe-*"
  // blocks) land at block-local coords instead of each insert location.
  int emitted = 0;
  std::shared_ptr<DRW_Polyline> pendingPoly;

  // Block names must match exactly between addBlock and addInsert (expandInserts
  // pairs them by name), so both go through the same conversion.
  auto blockName = [&]( Dwg_Object_BLOCK_HEADER *bh ) -> std::string {
    return bh ? toUtf8( bh->name, isTU ) : std::string();
  };

  auto emitEntity = [&]( Dwg_Object *obj ) {
    if ( !obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity )
      return;
    Dwg_Object_Entity *ent = obj->tio.entity;
    int err = 0;

    switch ( obj->fixedtype )
    {
      case DWG_TYPE_POINT:
      {
        Dwg_Entity_POINT *o = dwg_object_to_POINT( obj );
        DRW_Point e; fillCommon( e, obj, ent, isTU );
        e.basePoint.x = o->x; e.basePoint.y = o->y; e.basePoint.z = o->z;
        iface->addPoint( e ); ++emitted; break;
      }
      case DWG_TYPE_LINE:
      {
        Dwg_Entity_LINE *o = dwg_object_to_LINE( obj );
        DRW_Line e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->start ); e.secPoint = toCoord( o->end );
        iface->addLine( e ); ++emitted; break;
      }
      case DWG_TYPE_CIRCLE:
      {
        Dwg_Entity_CIRCLE *o = dwg_object_to_CIRCLE( obj );
        DRW_Circle e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->center ); e.radius = o->radius;
        iface->addCircle( e ); ++emitted; break;
      }
      case DWG_TYPE_ARC:
      {
        Dwg_Entity_ARC *o = dwg_object_to_ARC( obj );
        DRW_Arc e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->center ); e.radius = o->radius;
        e.staangle = o->start_angle; e.endangle = o->end_angle;
        iface->addArc( e ); ++emitted; break;
      }
      case DWG_TYPE_ELLIPSE:
      {
        Dwg_Entity_ELLIPSE *o = dwg_object_to_ELLIPSE( obj );
        DRW_Ellipse e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->center ); e.secPoint = toCoord( o->sm_axis );
        e.ratio = o->axis_ratio; e.staparam = o->start_angle; e.endparam = o->end_angle;
        iface->addEllipse( e ); ++emitted; break;
      }
      case DWG_TYPE_LWPOLYLINE:
      {
        Dwg_Entity_LWPOLYLINE *o = dwg_object_to_LWPOLYLINE( obj );
        DRW_LWPolyline e; fillCommon( e, obj, ent, isTU );
        e.flags = ( o->flag & 512 ) ? 1 : 0;
        e.elevation = o->elevation;
        for ( BITCODE_BL v = 0; v < o->num_points; ++v )
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
        pendingPoly = std::make_shared<DRW_Polyline>();
        fillCommon( *pendingPoly, obj, ent, isTU );
        pendingPoly->flags = o->flag;
        pendingPoly->basePoint.z = o->elevation;
        break;
      }
      case DWG_TYPE_VERTEX_2D:
      {
        if ( pendingPoly )
        {
          Dwg_Entity_VERTEX_2D *o = dwg_object_to_VERTEX_2D( obj );
          auto v = std::make_shared<DRW_Vertex>();
          v->basePoint = toCoord( o->point );
          v->bulge = o->bulge;
          pendingPoly->appendVertex( v );
        }
        break;
      }
      case DWG_TYPE_SEQEND:
      {
        if ( pendingPoly )
        {
          iface->addPolyline( *pendingPoly );
          ++emitted;
          pendingPoly.reset();
        }
        break;
      }
      case DWG_TYPE_SPLINE:
      {
        Dwg_Entity_SPLINE *o = dwg_object_to_SPLINE( obj );
        DRW_Spline e; fillCommon( e, obj, ent, isTU );
        e.degree = o->degree;
        e.flags = o->flag;
        e.tgStart = toCoord( o->beg_tan_vec );
        e.tgEnd = toCoord( o->end_tan_vec );
        for ( BITCODE_BL k = 0; k < o->num_knots; ++k )
          e.knotslist.push_back( o->knots[k] );
        for ( BITCODE_BL c = 0; c < o->num_ctrl_pts; ++c )
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
        DRW_Text e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord2( o->ins_pt ); e.basePoint.z = o->elevation;
        e.height = o->height;
        e.angle = o->rotation * 180.0 / M_PI;
        e.text = toUtf8( o->text_value, isTU );
        iface->addText( e ); ++emitted; break;
      }
      case DWG_TYPE_MTEXT:
      {
        Dwg_Entity_MTEXT *o = dwg_object_to_MTEXT( obj );
        DRW_MText e; fillCommon( e, obj, ent, isTU );
        e.basePoint = toCoord( o->ins_pt );
        e.height = o->text_height;
        e.text = toUtf8( o->text, isTU );
        iface->addMText( e ); ++emitted; break;
      }
      case DWG_TYPE_INSERT:
      {
        Dwg_Entity_INSERT *o = dwg_object_to_INSERT( obj );
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
        DRW_Hatch e; fillCommon( e, obj, ent, isTU );
        e.solid = o->is_solid_fill;
        e.associative = o->is_associative;
        e.angle = o->angle;
        e.scale = o->scale_spacing;
        // HATCH::name is decoded with FIELD_T (dwg.spec), so it is TU on R2007+
        // like every other BITCODE_T, despite being declared BITCODE_TV.
        e.name = toUtf8( o->name, isTU );
        e.loopsnum = o->num_paths;
        for ( BITCODE_BL p = 0; p < o->num_paths; ++p )
        {
          Dwg_HATCH_Path *path = &o->paths[p];
          auto loop = std::make_shared<DRW_HatchLoop>( static_cast<int>( path->flag ) );
          if ( path->flag & 2 ) // polyline boundary
          {
            addLwplBoundary( loop.get(), path );
          }
          else
          {
            for ( BITCODE_BL s = 0; s < path->num_segs_or_paths; ++s )
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
              // curve_type 3 (elliptic) / 4 (spline) boundary edges: TODO
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

  auto flushPoly = [&]() {
    if ( pendingPoly ) // POLYLINE without a trailing SEQEND
    {
      iface->addPolyline( *pendingPoly );
      pendingPoly.reset();
      ++emitted;
    }
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
    if ( !bh || !bh->entities || bh->num_owned == 0 )
      continue;
    DRW_Block db;
    db.name = blockName( bh );
    db.basePoint = toCoord( bh->base_pt );
    db.handle = static_cast<duint32>( obj->handle.value );
    iface->addBlock( db );
    for ( BITCODE_BL k = 0; k < bh->num_owned; ++k )
      emitEntity( dwg_ref_object( &dwg, bh->entities[k] ) );
    flushPoly();
    iface->endBlock();
  }

  // Model space: direct geometry plus the INSERT references that expandInserts
  // will resolve against the block definitions emitted above.
  if ( msObj && msObj->tio.object )
  {
    Dwg_Object_BLOCK_HEADER *bh = msObj->tio.object->tio.BLOCK_HEADER;
    if ( bh && bh->entities )
      for ( BITCODE_BL k = 0; k < bh->num_owned; ++k )
        emitEntity( dwg_ref_object( &dwg, bh->entities[k] ) );
  }
  flushPoly();

  QgsDebugMsgLevel( QStringLiteral( "LibreDWG: emitted %1 entities from %2 objects (version %3)" )
                      .arg( emitted ).arg( dwg.num_objects ).arg( static_cast<int>( dwg.header.version ) ), 2 );

  dwg_free( &dwg );
  mError = DRW::BAD_NONE;
  return true;
}
