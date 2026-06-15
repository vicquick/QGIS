# FindLibreDWG.cmake — locate the GNU LibreDWG library (https://www.gnu.org/software/libredwg/)
#
# qgis-ch issue #21, WP1 Option 2.
#
# Defines:
#   LibreDWG_FOUND
#   LibreDWG_INCLUDE_DIRS
#   LibreDWG_LIBRARIES
#   LibreDWG_VERSION        (when discoverable via pkg-config)
# and the imported target:
#   LibreDWG::LibreDWG
#
# Honours the LIBREDWG_ROOT hint.

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(PC_LibreDWG QUIET libredwg)
endif()

find_path(LibreDWG_INCLUDE_DIR
  NAMES dwg_api.h dwg.h
  HINTS ${LIBREDWG_ROOT} ${PC_LibreDWG_INCLUDEDIR} ${PC_LibreDWG_INCLUDE_DIRS}
  PATH_SUFFIXES include libredwg
)

find_library(LibreDWG_LIBRARY
  NAMES redwg libredwg
  HINTS ${LIBREDWG_ROOT} ${PC_LibreDWG_LIBDIR} ${PC_LibreDWG_LIBRARY_DIRS}
  PATH_SUFFIXES lib
)

set(LibreDWG_VERSION ${PC_LibreDWG_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibreDWG
  REQUIRED_VARS LibreDWG_LIBRARY LibreDWG_INCLUDE_DIR
  VERSION_VAR LibreDWG_VERSION
)

if (LibreDWG_FOUND)
  set(LibreDWG_LIBRARIES ${LibreDWG_LIBRARY})
  set(LibreDWG_INCLUDE_DIRS ${LibreDWG_INCLUDE_DIR})
  if (NOT TARGET LibreDWG::LibreDWG)
    add_library(LibreDWG::LibreDWG UNKNOWN IMPORTED)
    set_target_properties(LibreDWG::LibreDWG PROPERTIES
      IMPORTED_LOCATION "${LibreDWG_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LibreDWG_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(LibreDWG_INCLUDE_DIR LibreDWG_LIBRARY)
