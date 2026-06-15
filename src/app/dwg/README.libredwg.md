# DWG backend & Processing port — qgis-ch issue #21 (WP1 Option 2 + WP2)

Fork work for [qgis-ch sponsoring issue #21](https://github.com/qgis-ch/sponsoringprojects-projetsdesoutien-foerderprojekte/issues/21).
Branch: `feature/libredwg-backend`.

## Why

QGIS' bundled `libdxfrw` DWG reader hard-crashes (SIGSEGV in
`dwgCompressor::decompress18`, garbage compressed-page size) on some real-world
AutoCAD-2018 / Vectorworks-exported DWGs. GNU **LibreDWG** parses r13–r2018 at
~99% and *skips* entities it cannot read instead of crashing.

## WP1 — LibreDWG backend (Strategy B: native adapter)

Key architectural fact: `QgsDwgImporter` **is a** `DRW_Interface` (libdxfrw's
abstract sink). libdxfrw's `dwgR::read(this)` simply fires ~50 `add*()`
callbacks. Every QGIS-specific behaviour (colour/linetype/transparency/MTEXT
fixes, GPKG writing, INSERT expansion, curve handling) lives in that sink and
is **parser-agnostic**.

So we do not touch `QgsDwgImporter`. We add a drop-in reader with the same
shape as `dwgR`:

| | libdxfrw | LibreDWG backend |
|---|---|---|
| class | `dwgR` | `QgsLibreDwgReader` |
| call | `dwg.read(this, true)` | `reader.read(this, true)` |
| error | `dwg.getError()` | `reader.error()` |
| version | `dwg.getVersion()` | `reader.version()` |

`QgsLibreDwgReader` (`qgslibredwgreader.{h,cpp}`) calls `dwg_read_file()`, then
walks the `Dwg_Data` object array and synthesises `DRW_*` structs, replaying
the same callbacks libdxfrw would. Swap point: `qgsdwgimporter.cpp` DWG branch,
behind `#ifdef WITH_LIBREDWG` + the runtime setting `cad/dwgBackend`
(`"libdxfrw"` default | `"libredwg"`).

### Status
- ✅ **Compiled + symbol-checked against GNU LibreDWG 0.13.4.8295** (latest,
  2026-06-15). Uses stable struct-field access, not the `USE_DEPRECATED_API`
  per-field getters.
- ✅ Entity coverage validated against a real 34 MB Vectorworks export
  (966k objects): **LINE, POLYLINE_2D (+VERTEX_2D/SEQEND state machine),
  LWPOLYLINE, SPLINE, HATCH (line/arc/polyline boundaries), SOLID, MTEXT,
  INSERT** — every graphic type that file contains — plus the common set
  POINT/CIRCLE/ARC/ELLIPSE/TEXT and the LAYER table.
- ✅ Backend toggle, CMake `find_package(LibreDWG)` + `FindLibreDWG.cmake`,
  `WITH_LIBREDWG` guard. Default build path **unchanged**.
- ⬜ HATCH boundary edges of curve_type 3 (elliptic) / 4 (spline); WIPEOUT,
  VIEWPORT, 3DFACE, TRACE, DIMENSION_*, LEADER, MLINE, IMAGE, RAY, XLINE.
- ⬜ True-colour (RGB) + transparency mapping, block-scoped INSERT traversal,
  linetype table emission.
- ⬜ Regression tests with sample DWGs per ACAD version.

### License
`QgsDwgImporter` is GPLv2-**or-later**; LibreDWG is GPLv3-**or-later**. The
combined binary is therefore GPLv3-or-later — compatible. (GPLv2-*only* would
not be; the "or later" is what permits the link.) The importer lives in
`src/app` — the GPL application — not in a library meant for non-GPL linkers.

## WP2 — DWG/DXF → GeoPackage as a Processing algorithm

`QgsDwgToGpkgAlgorithm` (`processing/qgsdwgtogpkgalgorithm.{h,cpp}`) wraps the
same `QgsDwgImporter` so the conversion — previously only reachable from the
`QgsDwgImportDialog` GUI — works from `qgis_process`, batch mode and graphical
models. Parameters: INPUT (dwg/dxf), CRS, EXPAND_INSERTS, USE_CURVES, OUTPUT
(gpkg). It honours the same `cad/dwgBackend` setting.

### Status / follow-ups
- ✅ Algorithm class + parameters + `processAlgorithm` calling `QgsDwgImporter`.
- ⬜ **Registration**: as an application-tier algorithm it must be added to the
  QGIS app processing provider. The clean long-term home is `qgis_analysis` as
  a *native* algorithm, which additionally requires relocating `QgsDwgImporter`
  out of `src/app` into a linkable library (core/analysis). Documented here as
  the WP2 phase-2 task.
- ⬜ Layer-merge option parity with the dialog; per-feature progress via
  `QgsProcessingFeedback`.

## Build

```bash
# Debian/Ubuntu
apt-get install libredwg-dev        # >= 0.13.4
cmake -DWITH_LIBREDWG=ON ...        # auto-on when LibreDWG is found
```

Then in QGIS: **Settings ▸ Advanced ▸ `cad/dwgBackend` = `libredwg`** (a proper
Options-dialog UI control is a small follow-up), and import the DWG as usual.
```
