/***************************************************************************
                         qgsdwgtogpkgalgorithm.h
                         -----------------------
    begin                : June 2026
    copyright            : (C) 2026 by Victor Budinich

    qgis-ch issue #21, WP2 — expose the DWG/DXF -> GeoPackage conversion
    (previously GUI-only, in QgsDwgImportDialog) as a Processing algorithm so
    it is reachable from qgis_process, batch mode and graphical models.
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QGSDWGTOGPKGALGORITHM_H
#define QGSDWGTOGPKGALGORITHM_H

#include "qgsprocessingalgorithm.h"

/**
 * \brief Converts a DWG/DXF drawing into a GeoPackage of layered CAD features.
 *
 * Thin Processing wrapper around QgsDwgImporter (the same engine the
 * "Project > Import/Export > Import DWG/DXF" dialog drives). Because it is an
 * application-tier algorithm it must be registered with the QGIS application
 * processing provider (see README.libredwg.md, WP2 notes). The longer-term
 * clean home is qgis_analysis as a native algorithm, which additionally
 * requires relocating QgsDwgImporter out of src/app.
 */
class QgsDwgToGpkgAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QgsDwgToGpkgAlgorithm() = default;

    QString name() const override { return QStringLiteral( "dwgtogpkg" ); }
    QString displayName() const override { return QObject::tr( "Convert DWG/DXF to GeoPackage" ); }
    QStringList tags() const override { return QObject::tr( "dwg,dxf,cad,import,convert,geopackage,gpkg,autocad" ).split( ',' ); }
    QString group() const override { return QObject::tr( "Vector general" ); }
    QString groupId() const override { return QStringLiteral( "vectorgeneral" ); }
    QString shortHelpString() const override;
    QgsDwgToGpkgAlgorithm *createInstance() const override SIP_FACTORY;

  protected:
    void initAlgorithm( const QVariantMap &configuration = QVariantMap() ) override;
    QVariantMap processAlgorithm( const QVariantMap &parameters,
                                  QgsProcessingContext &context,
                                  QgsProcessingFeedback *feedback ) override;
};

#endif // QGSDWGTOGPKGALGORITHM_H
