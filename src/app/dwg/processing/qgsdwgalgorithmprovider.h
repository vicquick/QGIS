/***************************************************************************
                         qgsdwgalgorithmprovider.h
                         -------------------------
    begin                : August 2026
    copyright            : (C) 2026 by Victor Budinich
    email                : victor at budinic dot art
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QGSDWGALGORITHMPROVIDER_H
#define QGSDWGALGORITHMPROVIDER_H

#include "qgsprocessingprovider.h"

/**
 * \ingroup app
 * \class QgsDwgAlgorithmProvider
 * \brief Processing provider for the CAD import algorithms.
 *
 * QgsDwgToGpkgAlgorithm lives in src/app because it drives QgsDwgImporter,
 * which is app code, so it cannot join QgsNativeAlgorithms in core. Without a
 * provider of its own the class compiled but was never instantiated, leaving
 * the algorithm unreachable from qgis_process, the toolbox and models.
 */
class QgsDwgAlgorithmProvider : public QgsProcessingProvider
{
    Q_OBJECT

  public:
    QgsDwgAlgorithmProvider( QObject *parent = nullptr );

    QString id() const override;
    QString name() const override;
    QIcon icon() const override;
    QString svgIconPath() const override;
    bool supportsNonFileBasedOutput() const override { return false; }

  protected:
    void loadAlgorithms() override;
};

#endif // QGSDWGALGORITHMPROVIDER_H
