/***************************************************************************
                         qgslibredwgreader.h
                         -------------------
    begin                : June 2026
    copyright            : (C) 2026 by Victor Budinich
    email                : victor.budinich at gmail dot com

    WP1 (Option 2) of qgis-ch sponsoring issue #21:
    an alternative DWG reader backend built on GNU LibreDWG that emits the
    exact same DRW_Interface callbacks as libdxfrw's dwgR, so QgsDwgImporter
    (and every QGIS-specific fix it carries) is reused verbatim.
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   NOTE: the resulting binary links GNU LibreDWG (GPLv3-or-later); the   *
 *   combined work is therefore distributed under GPLv3-or-later, which is *
 *   compatible with QGIS' GPLv2-or-later application code.                *
 ***************************************************************************/

#ifndef QGSLIBREDWGREADER_H
#define QGSLIBREDWGREADER_H

#include <string>

#include "drw_base.h"       // DRW::error, DRW::Version
#include "drw_interface.h"  // DRW_Interface (the sink QgsDwgImporter implements)

/**
 * \brief Reads a DWG drawing with GNU LibreDWG and replays its contents
 * through the libdxfrw DRW_Interface callback vocabulary.
 *
 * Drop-in alternative to libdxfrw's \c dwgR: same read()/error()/version()
 * shape, so the call site in QgsDwgImporter::import() only has to pick a
 * backend. The reader owns no QGIS state; it is a pure parser->callback
 * adapter.
 *
 * Strategy B from the issue #21 research: LibreDWG parses the whole file
 * into its in-memory Dwg_Data object tree, and this class walks that tree
 * synthesising DRW_* structs. libdxfrw stays the lingua franca; only the
 * DWG byte-parser is replaced (and with it the crash-prone decompress18
 * path in libdxfrw's dwgReader18).
 */
class QgsLibreDwgReader
{
  public:
    explicit QgsLibreDwgReader( const std::string &fileName );
    ~QgsLibreDwgReader();

    QgsLibreDwgReader( const QgsLibreDwgReader & ) = delete;
    QgsLibreDwgReader &operator=( const QgsLibreDwgReader & ) = delete;

    /**
     * Parse the file and emit callbacks on \a iface.
     * \param iface the sink (QgsDwgImporter)
     * \param expandInserts kept for signature-parity with dwgR::read(); INSERTs
     *        are always emitted as DRW_Insert and expanded downstream by
     *        QgsDwgImporter, exactly as with libdxfrw.
     * \returns true on success.
     */
    bool read( DRW_Interface *iface, bool expandInserts );

    //! Error status, mapped onto libdxfrw's DRW::error so the caller is backend-agnostic.
    DRW::error error() const { return mError; }

    //! Drawing version, mapped onto libdxfrw's DRW::Version.
    DRW::Version version() const { return mVersion; }

  private:
    std::string mFileName;
    DRW::error mError = DRW::BAD_NONE;
    DRW::Version mVersion = DRW::UNKNOWNV;
};

#endif // QGSLIBREDWGREADER_H
