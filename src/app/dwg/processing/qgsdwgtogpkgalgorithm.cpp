/***************************************************************************
                         qgsdwgtogpkgalgorithm.cpp
                         -------------------------
    begin                : June 2026
    copyright            : (C) 2026 by Victor Budinich

    qgis-ch issue #21, WP2 — DWG/DXF -> GeoPackage Processing algorithm.
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsdwgtogpkgalgorithm.h"
#include "qgsdwgimporter.h"

#include "qgsprocessingparameters.h"
#include "qgscoordinatereferencesystem.h"

#include <QFileInfo>

void QgsDwgToGpkgAlgorithm::initAlgorithm( const QVariantMap & )
{
  addParameter( new QgsProcessingParameterFile(
    QStringLiteral( "INPUT" ), QObject::tr( "Source DWG/DXF drawing" ),
    Qgis::ProcessingFileParameterBehavior::File,
    QString(), QVariant(), false,
    QObject::tr( "CAD drawings (*.dwg *.DWG *.dxf *.DXF)" ) ) );

  addParameter( new QgsProcessingParameterCrs(
    QStringLiteral( "CRS" ), QObject::tr( "Target CRS" ),
    QStringLiteral( "ProjectCrs" ) ) );

  addParameter( new QgsProcessingParameterBoolean(
    QStringLiteral( "EXPAND_INSERTS" ), QObject::tr( "Expand block references (INSERTs)" ), true ) );

  addParameter( new QgsProcessingParameterBoolean(
    QStringLiteral( "USE_CURVES" ), QObject::tr( "Keep curved geometries (arcs/circles) as curves" ), true ) );

  addParameter( new QgsProcessingParameterFileDestination(
    QStringLiteral( "OUTPUT" ), QObject::tr( "Output GeoPackage" ),
    QObject::tr( "GeoPackage (*.gpkg *.GPKG)" ) ) );
}

QString QgsDwgToGpkgAlgorithm::shortHelpString() const
{
  return QObject::tr(
    "Converts an AutoCAD DWG or DXF drawing into a GeoPackage, with one layer "
    "per CAD geometry type (points, lines, polygons, hatches, text labels, "
    "inserts). This is the same conversion offered by Project ▸ Import/Export ▸ "
    "Import DWG/DXF, made available to qgis_process, batch runs and models.\n\n"
    "The DWG read backend (libdxfrw or GNU LibreDWG) follows the global "
    "Settings ▸ CAD ▸ DWG backend option when QGIS is built with LibreDWG "
    "support." );
}

QgsDwgToGpkgAlgorithm *QgsDwgToGpkgAlgorithm::createInstance() const
{
  return new QgsDwgToGpkgAlgorithm();
}

QVariantMap QgsDwgToGpkgAlgorithm::processAlgorithm( const QVariantMap &parameters,
    QgsProcessingContext &context, QgsProcessingFeedback *feedback )
{
  const QString input = parameterAsString( parameters, QStringLiteral( "INPUT" ), context );
  const QString output = parameterAsFileOutput( parameters, QStringLiteral( "OUTPUT" ), context );
  const QgsCoordinateReferenceSystem crs = parameterAsCrs( parameters, QStringLiteral( "CRS" ), context );
  const bool expandInserts = parameterAsBool( parameters, QStringLiteral( "EXPAND_INSERTS" ), context );
  const bool useCurves = parameterAsBool( parameters, QStringLiteral( "USE_CURVES" ), context );

  const QString suffix = QFileInfo( input ).suffix().toLower();
  if ( suffix != QLatin1String( "dwg" ) && suffix != QLatin1String( "dxf" ) )
    throw QgsProcessingException( QObject::tr( "Input must be a .dwg or .dxf file (got '%1')." ).arg( input ) );

  if ( feedback )
    feedback->pushInfo( QObject::tr( "Converting %1 → %2" ).arg( input, output ) );

  QgsDwgImporter importer( output, crs );
  QString error;
  // QLabel* progress sink is GUI-only; Processing reports via feedback instead.
  const bool ok = importer.import( input, error, expandInserts, useCurves, nullptr );
  if ( !ok )
    throw QgsProcessingException( QObject::tr( "DWG/DXF import failed: %1" ).arg( error ) );

  if ( feedback )
    feedback->setProgress( 100.0 );

  QVariantMap results;
  results.insert( QStringLiteral( "OUTPUT" ), output );
  return results;
}
