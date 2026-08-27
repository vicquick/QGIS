/***************************************************************************
                         qgsdwgalgorithmprovider.cpp
                         ---------------------------
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

#include "qgsdwgalgorithmprovider.h"
#include "qgsdwgtogpkgalgorithm.h"
#include "qgsapplication.h"

QgsDwgAlgorithmProvider::QgsDwgAlgorithmProvider( QObject *parent )
  : QgsProcessingProvider( parent )
{
}

QString QgsDwgAlgorithmProvider::id() const
{
  return QStringLiteral( "cad" );
}

QString QgsDwgAlgorithmProvider::name() const
{
  return QObject::tr( "CAD" );
}

QIcon QgsDwgAlgorithmProvider::icon() const
{
  return QgsApplication::getThemeIcon( QStringLiteral( "/mIconVector.svg" ) );
}

QString QgsDwgAlgorithmProvider::svgIconPath() const
{
  return QgsApplication::iconPath( QStringLiteral( "mIconVector.svg" ) );
}

void QgsDwgAlgorithmProvider::loadAlgorithms()
{
  addAlgorithm( new QgsDwgToGpkgAlgorithm() );
}
