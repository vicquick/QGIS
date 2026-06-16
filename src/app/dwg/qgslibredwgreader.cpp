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
#include <memory>

// GNU LibreDWG public API (>= 0.13.4). Headers ship in libredwg-dev / our build.
extern "C"
{
#include <dwg.h>
#include <dwg_api.h>
}

namespace
{
  // BITCODE_3BD / BITCODE_3DPOINT are {x,y,z}; BITCODE_2RD / BITCODE_2DPOINT are {x,y}.
  template <typename P> DRW_Coord toCoord( const P &p ) { DRW_Coord c; c.x = p.x; c.y = p.y; c.z = p.z; return c; }
  template <typename P> DRW_Coord toCoord2( const P &p ) { DRW_Coord c; c.x = p.x; c.y = p.y; c.z = 0; return c; }

  // Normalise a LibreDWG linetype name to what QgsDwgImporter::linetypeString expects:
  // "bylayer"/"byblock" lowercased (special-cased there); other names match the
  // LTYPE table emitted via addLType(); CONTINUOUS -> empty == solid.
  std::string normLineType( const char *lt )
  {
    if ( !lt )
      return std::string();
    std::string s( lt );
    if ( strcasecmp( s.c_str(), "BYLAYER" ) == 0 ) return std::string( "bylayer" );
    if ( strcasecmp( s.c_str(), "BYBLOCK" ) == 0 ) return std::string( "byblock" );
    if ( strcasecmp( s.c_str(), "CONTINUOUS" ) == 0 ) return std::string();
    return s;
  }

  void fillCommon( DRW_Entity &e, Dwg_Object *obj, Dwg_Object_Entity *ent )
  {
    int err = 0;
    if ( char *layer = dwg_ent_get_layer_name( ent, &err ) )
      if ( !err && layer )
        e.layer = std::string( layer );
    if ( const Dwg_Color *col = dwg_ent_get_color( ent, &err ) )
    {
      e.color = col->index;          // ACI; 256 == BYLAYER, 0 == BYBLOCK
      // LibreDWG packs the colour method in the top byte of rgb: 0x02/0xC2 == true
      // RGB; 0xC0/0xC1 == ByLayer/ByBlock and must NOT be read as an explicit colour.
      const unsigned method = ( static_cast<unsigned>( col->rgb ) >> 24 ) & 0xffu;
      e.color24 = ( method == 0x02u || method == 0xC2u ) ? static_cast<int>( col->rgb & 0xffffffu ) : -1;
    }
    // Entity lineweight: LibreDWG's linewt byte uses the same index encoding as
    // DRW_LW_Conv::lineWidth (2 == 0.09mm, 7 == 0.25mm, 29 == ByLayer ...).
    e.lWeight = static_cast<DRW_LW_Conv::lineWidth>( static_cast<signed char>( ent->linewt ) );
    // Entity linetype (resolves ByLayer/ByBlock/Continuous/explicit internally).
    err = 0;
    if ( char *lt = dwg_ent_get_ltype_name( ent, &err ) )
      if ( !err )
        e.lineType = normLineType( lt );
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

  DRW::Version mapVersion( Dwg_Version_Type v )
  {
    switch ( v )
    {
      case R_2000: return DRW::AC1015;
      case R_2004: return DRW::AC1018;
      case R_2007: return DRW::AC1021;
      case R_2010: return DRW::AC1024;
      case R_2013: return DRW::AC1027;
      case R_2018: return DRW::AC1032;
      default:     return DRW::UNKNOWNV;
    }
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

  // First pass: symbol tables — LAYER (BYLAYER colour/lineweight refs) and LTYPE
  // (dash definitions, so QgsDwgImporter::linetypeString can resolve dashes).
  for ( BITCODE_BL i = 0; i < dwg.num_objects; ++i )
  {
    Dwg_Object *obj = &dwg.object[i];
    if ( obj->supertype != DWG_SUPERTYPE_OBJECT )
      continue;

    if ( obj->fixedtype == DWG_TYPE_LAYER )
    {
      Dwg_Object_LAYER *lay = dwg_object_to_LAYER( obj );
      if ( !lay )
        continue;
      DRW_Layer dl;
      int err = 0;
      if ( char *nm = dwg_obj_layer_get_name( lay, &err ) )
        if ( !err && nm )
          dl.name = std::string( nm );
      dl.color = lay->color.index;
      const unsigned m = ( static_cast<unsigned>( lay->color.rgb ) >> 24 ) & 0xffu;
      dl.color24 = ( m == 0x02u || m == 0xC2u ) ? static_cast<int>( lay->color.rgb & 0xffffffu ) : -1;
      dl.lWeight = static_cast<DRW_LW_Conv::lineWidth>( static_cast<signed char>( lay->linewt ) );
      iface->addLayer( dl );
    }
    else if ( obj->fixedtype == DWG_TYPE_LTYPE )
    {
      Dwg_Object_LTYPE *lt = dwg_object_to_LTYPE( obj );
      if ( !lt )
        continue;
      DRW_LType dl;
      int err = 0;
      if ( char *nm = dwg_obj_table_get_name( obj, &err ) )
        if ( !err && nm )
          dl.name = std::string( nm );
      for ( BITCODE_RC d = 0; d < lt->numdashes; ++d )
        dl.path.push_back( lt->dashes[d].length );
      iface->addLType( dl );
    }
  }

  // Second pass: entities. POLYLINE_2D / VERTEX_2D / SEQEND are emitted as a
  // single DRW_Polyline accumulated across the contiguous object run, matching
  // how libdxfrw's dwgReader feeds QgsDwgImporter.
  int emitted = 0;
  std::shared_ptr<DRW_Polyline> pendingPoly;

  for ( BITCODE_BL i = 0; i < dwg.num_objects; ++i )
  {
    Dwg_Object *obj = &dwg.object[i];
    if ( obj->supertype != DWG_SUPERTYPE_ENTITY )
      continue;
    Dwg_Object_Entity *ent = obj->tio.entity;
    int err = 0;

    switch ( obj->fixedtype )
    {
      case DWG_TYPE_POINT:
      {
        Dwg_Entity_POINT *o = dwg_object_to_POINT( obj );
        DRW_Point e; fillCommon( e, obj, ent );
        e.basePoint.x = o->x; e.basePoint.y = o->y; e.basePoint.z = o->z;
        iface->addPoint( e ); ++emitted; break;
      }
      case DWG_TYPE_LINE:
      {
        Dwg_Entity_LINE *o = dwg_object_to_LINE( obj );
        DRW_Line e; fillCommon( e, obj, ent );
        e.basePoint = toCoord( o->start ); e.secPoint = toCoord( o->end );
        iface->addLine( e ); ++emitted; break;
      }
      case DWG_TYPE_CIRCLE:
      {
        Dwg_Entity_CIRCLE *o = dwg_object_to_CIRCLE( obj );
        DRW_Circle e; fillCommon( e, obj, ent );
        e.basePoint = toCoord( o->center ); e.radius = o->radius;
        iface->addCircle( e ); ++emitted; break;
      }
      case DWG_TYPE_ARC:
      {
        Dwg_Entity_ARC *o = dwg_object_to_ARC( obj );
        DRW_Arc e; fillCommon( e, obj, ent );
        e.basePoint = toCoord( o->center ); e.radius = o->radius;
        e.staangle = o->start_angle; e.endangle = o->end_angle;
        iface->addArc( e ); ++emitted; break;
      }
      case DWG_TYPE_ELLIPSE:
      {
        Dwg_Entity_ELLIPSE *o = dwg_object_to_ELLIPSE( obj );
        DRW_Ellipse e; fillCommon( e, obj, ent );
        e.basePoint = toCoord( o->center ); e.secPoint = toCoord( o->sm_axis );
        e.ratio = o->axis_ratio; e.staparam = o->start_angle; e.endparam = o->end_angle;
        iface->addEllipse( e ); ++emitted; break;
      }
      case DWG_TYPE_LWPOLYLINE:
      {
        Dwg_Entity_LWPOLYLINE *o = dwg_object_to_LWPOLYLINE( obj );
        DRW_LWPolyline e; fillCommon( e, obj, ent );
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
        fillCommon( *pendingPoly, obj, ent );
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
        DRW_Spline e; fillCommon( e, obj, ent );
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
        DRW_Solid e; fillCommon( e, obj, ent );
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
        DRW_Text e; fillCommon( e, obj, ent );
        e.basePoint = toCoord2( o->ins_pt ); e.basePoint.z = o->elevation;
        e.height = o->height;
        e.angle = o->rotation * 180.0 / M_PI;
        if ( o->text_value )
          e.text = std::string( reinterpret_cast<char *>( o->text_value ) );
        iface->addText( e ); ++emitted; break;
      }
      case DWG_TYPE_MTEXT:
      {
        Dwg_Entity_MTEXT *o = dwg_object_to_MTEXT( obj );
        DRW_MText e; fillCommon( e, obj, ent );
        e.basePoint = toCoord( o->ins_pt );
        e.height = o->text_height;
        if ( o->text )
          e.text = std::string( reinterpret_cast<char *>( o->text ) );
        iface->addMText( e ); ++emitted; break;
      }
      case DWG_TYPE_INSERT:
      {
        Dwg_Entity_INSERT *o = dwg_object_to_INSERT( obj );
        DRW_Insert e; fillCommon( e, obj, ent );
        e.basePoint = toCoord( o->ins_pt );
        e.xscale = o->scale.x; e.yscale = o->scale.y; e.zscale = o->scale.z;
        e.angle = o->rotation;
        if ( o->block_name )
          e.name = std::string( reinterpret_cast<char *>( o->block_name ) );
        iface->addInsert( e ); ++emitted; break;
      }
      case DWG_TYPE_HATCH:
      {
        Dwg_Entity_HATCH *o = dwg_object_to_HATCH( obj );
        DRW_Hatch e; fillCommon( e, obj, ent );
        e.solid = o->is_solid_fill;
        e.associative = o->is_associative;
        e.angle = o->angle;
        e.scale = o->scale_spacing;
        if ( o->name )
          e.name = std::string( reinterpret_cast<char *>( o->name ) );
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
  }

  if ( pendingPoly ) // POLYLINE without a trailing SEQEND
  {
    iface->addPolyline( *pendingPoly );
    ++emitted;
  }

  QgsDebugMsgLevel( QStringLiteral( "LibreDWG: emitted %1 entities from %2 objects (version %3)" )
                      .arg( emitted ).arg( dwg.num_objects ).arg( static_cast<int>( dwg.header.version ) ), 2 );

  dwg_free( &dwg );
  mError = DRW::BAD_NONE;
  return true;
}
