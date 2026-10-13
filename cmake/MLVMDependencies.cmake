# Resolve dependencies relative to the separated libstone checkout.  This file
# intentionally does not look above the repository root; populate
# libstone/third_party or pass explicit *_SOURCE_DIR
# overrides.  The historical MLVM_* cache names remain as aliases so existing
# component CMake files and downstream invocations keep working.
get_filename_component(LIBSTONE_DEFAULT_THIRD_PARTY_DIR
  "${CMAKE_CURRENT_LIST_DIR}/../third_party" ABSOLUTE)

if(DEFINED MLVM_THIRD_PARTY_DIR AND NOT DEFINED LIBSTONE_THIRD_PARTY_DIR)
  set(LIBSTONE_THIRD_PARTY_DIR "${MLVM_THIRD_PARTY_DIR}" CACHE PATH
    "libstone dependency source directory")
else()
  set(LIBSTONE_THIRD_PARTY_DIR "${LIBSTONE_DEFAULT_THIRD_PARTY_DIR}"
    CACHE PATH "libstone dependency source directory")
endif()
set(MLVM_THIRD_PARTY_DIR "${LIBSTONE_THIRD_PARTY_DIR}" CACHE PATH
  "Compatibility alias for LIBSTONE_THIRD_PARTY_DIR" FORCE)

function(libstone_dependency_source dependency key)
  set(_default "${LIBSTONE_THIRD_PARTY_DIR}/${dependency}")
  set(_libstone_var "LIBSTONE_${key}_SOURCE_DIR")
  set(_mlvm_var "MLVM_${key}_SOURCE_DIR")
  if(DEFINED ${_mlvm_var} AND NOT DEFINED ${_libstone_var})
    set(${_libstone_var} "${${_mlvm_var}}" CACHE PATH
      "${dependency} source directory")
  else()
    set(${_libstone_var} "${_default}" CACHE PATH
      "${dependency} source directory")
  endif()
  set(${_mlvm_var} "${${_libstone_var}}" CACHE PATH
    "Compatibility alias for ${_libstone_var}" FORCE)
endfunction()

libstone_dependency_source(sfsexp SFSEXP)

foreach(dependency IN ITEMS dparser metatk satie equinox-ng exolangtk
    lmdbxx kaguya lua klib tomlc99 isocline termlib)
  string(TOUPPER "${dependency}" key)
  string(REPLACE "-" "_" key "${key}")
  if(key STREQUAL "TOMLC99")
    set(key TOML)
  endif()
  libstone_dependency_source("${dependency}" "${key}")
endforeach()
# Satie and VMWeave use the DomMEMTk API bundled with Satie. Upstream
# memtkx has since renamed that API; use the matching snapshot explicitly.
if(DEFINED MLVM_MEMTKX_SOURCE_DIR AND NOT DEFINED LIBSTONE_MEMTKX_SOURCE_DIR)
  set(LIBSTONE_MEMTKX_SOURCE_DIR "${MLVM_MEMTKX_SOURCE_DIR}" CACHE PATH
    "Satie-compatible DomMEMTk headers")
else()
  set(LIBSTONE_MEMTKX_SOURCE_DIR "${LIBSTONE_SATIE_SOURCE_DIR}/third_party/memtkx"
    CACHE PATH "Satie-compatible DomMEMTk headers")
endif()
set(MLVM_MEMTKX_SOURCE_DIR "${LIBSTONE_MEMTKX_SOURCE_DIR}" CACHE PATH
  "Compatibility alias for LIBSTONE_MEMTKX_SOURCE_DIR" FORCE)

set(MLVM_SFSEXP_SOURCES)
foreach(source cstring.c event_temp.c faststack.c io.c parser.c sexp.c
                sexp_memory.c sexp_ops.c sexp_vis.c)
  list(APPEND MLVM_SFSEXP_SOURCES "${MLVM_SFSEXP_SOURCE_DIR}/src/${source}")
endforeach()
set(LIBSTONE_SFSEXP_SOURCES ${MLVM_SFSEXP_SOURCES})
set(MLVM_SFSEXP_AVAILABLE ON)
set(MLVM_SFSEXP_MISSING_FILE "")
foreach(file IN LISTS MLVM_SFSEXP_SOURCES ITEMS
    "${MLVM_SFSEXP_SOURCE_DIR}/src/sexp.h"
    "${MLVM_SFSEXP_SOURCE_DIR}/src/sexp_errors.h"
    "${MLVM_SFSEXP_SOURCE_DIR}/src/sexp_memory.h"
    "${MLVM_SFSEXP_SOURCE_DIR}/src/sexp_ops.h"
    "${MLVM_SFSEXP_SOURCE_DIR}/src/sexp_vis.h"
    "${MLVM_SFSEXP_SOURCE_DIR}/src/cstring.h"
    "${MLVM_SFSEXP_SOURCE_DIR}/src/faststack.h")
  if(NOT EXISTS "${file}")
    set(MLVM_SFSEXP_AVAILABLE OFF)
    set(MLVM_SFSEXP_MISSING_FILE "${file}")
    break()
  endif()
endforeach()
