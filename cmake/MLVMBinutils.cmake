# Build only the library sources needed by the adapter. This deliberately
# avoids configuring libstone's independent CLIs, generators and dependencies.
# External deps: third_party/klib (ketopt), third_party/tomlc99 (toml)
# Limestone itself is checked in under libstone/limestone.
if(NOT DEFINED LIBSTONE_SOURCE_DIR)
  get_filename_component(LIBSTONE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
set(_libstone_limestone "${LIBSTONE_SOURCE_DIR}/limestone")
set(MLVM_ENABLE_BINUTILS AUTO CACHE STRING "Library-backed binary tools: AUTO, ON, or OFF")
set_property(CACHE MLVM_ENABLE_BINUTILS PROPERTY STRINGS AUTO ON OFF)
if(NOT MLVM_ENABLE_BINUTILS MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "MLVM_ENABLE_BINUTILS must be AUTO, ON, or OFF")
endif()
if(NOT MLVM_ENABLE_BINUTILS STREQUAL "OFF")
  set(MLVM_KLIB_SOURCE_DIR "${MLVM_THIRD_PARTY_DIR}/klib"
    CACHE PATH "klib source directory")
  set(MLVM_TOML_SOURCE_DIR "${MLVM_THIRD_PARTY_DIR}/tomlc99"
    CACHE PATH "tomlc99 source directory")

  foreach(source "${_libstone_limestone}/bin2bin/object.cpp" "${_libstone_limestone}/bin2bin/archive.cpp"
                 "${_libstone_limestone}/bin2bin/object.hpp" "${_libstone_limestone}/metacode/metacode.hpp"
                 "${_libstone_limestone}/limestone/foundation.hpp"
                 "${MLVM_KLIB_SOURCE_DIR}/ketopt.h"
                  "${MLVM_TOML_SOURCE_DIR}/toml.c" "${MLVM_TOML_SOURCE_DIR}/toml.h")
    if(NOT IS_ABSOLUTE "${source}")
      set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    endif()
    if(NOT EXISTS "${source}")
      if(MLVM_ENABLE_BINUTILS STREQUAL "ON")
        message(FATAL_ERROR "Library-backed binary tools require ${source}")
      endif()
      set(MLVM_ENABLE_BINUTILS OFF)
      message(STATUS "Library-backed binary tools disabled: ${source} unavailable")
      break()
    endif()
  endforeach()
endif()
if(NOT MLVM_ENABLE_BINUTILS STREQUAL "OFF")
  foreach(source src/binutils/adapter.cpp src/binutils/archive.cpp src/binutils/contract.cpp src/binutils/transform.cpp)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
      if(MLVM_ENABLE_BINUTILS STREQUAL "ON")
        message(FATAL_ERROR "Library-backed binary tools require ${CMAKE_CURRENT_SOURCE_DIR}/${source}")
      endif()
      set(MLVM_ENABLE_BINUTILS OFF)
      message(STATUS "Library-backed binary tools disabled in separated libstone: ${source} unavailable")
      break()
    endif()
  endforeach()
endif()
if(NOT MLVM_ENABLE_BINUTILS STREQUAL "OFF")
  set(MLVM_ENABLE_BINUTILS ON)
  add_library(mlvm_stone_objects STATIC "${_libstone_limestone}/bin2bin/object.cpp"
    "${_libstone_limestone}/bin2bin/archive.cpp")
  target_include_directories(mlvm_stone_objects PUBLIC
    $<BUILD_INTERFACE:${_libstone_limestone}>)
  target_compile_features(mlvm_stone_objects PUBLIC cxx_std_20)
  set_target_properties(mlvm_stone_objects PROPERTIES EXPORT_NAME StoneObjects POSITION_INDEPENDENT_CODE ON)
  add_library(mlvm_toml STATIC "${MLVM_TOML_SOURCE_DIR}/toml.c")
  target_include_directories(mlvm_toml PUBLIC
    $<BUILD_INTERFACE:${MLVM_TOML_SOURCE_DIR}>)
  set_target_properties(mlvm_toml PROPERTIES C_STANDARD 99 EXPORT_NAME TOML POSITION_INDEPENDENT_CODE ON)
  add_library(mlvm_binutils src/binutils/adapter.cpp src/binutils/archive.cpp
    src/binutils/contract.cpp src/binutils/transform.cpp)
  add_library(MLVM::Binutils ALIAS mlvm_binutils)
  target_link_libraries(mlvm_binutils PUBLIC MLVM::Object PRIVATE mlvm_stone_objects mlvm_toml)
  set_target_properties(mlvm_binutils PROPERTIES EXPORT_NAME Binutils)
  mlvm_warnings(mlvm_binutils)
  install(TARGETS mlvm_binutils mlvm_stone_objects mlvm_toml EXPORT MLVMTargets)
  install(FILES "${_libstone_limestone}/LICENSE" DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses RENAME Limestone-LICENSE)
  install(FILES "${MLVM_TOML_SOURCE_DIR}/LICENSE" DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses RENAME tomlc99-LICENSE)
  install(FILES "${MLVM_KLIB_SOURCE_DIR}/LICENSE.txt" DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses RENAME klib-LICENSE)
  install(DIRECTORY examples/targets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/mlvm/targets)
endif()
