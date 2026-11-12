cmake_minimum_required(VERSION 3.20)

# The version lives in include/memtkx/core/version.h (single source of truth).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/memtkx/core/version.h" _mtkx_version_lines
     REGEX "^#define MTKX_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(_line IN LISTS _mtkx_version_lines)
  if(_line MATCHES "^#define MTKX_VERSION_(MAJOR|MINOR|PATCH) +([0-9]+)")
    set(_mtkx_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
  endif()
endforeach()

project(MemTKX
  VERSION ${_mtkx_MAJOR}.${_mtkx_MINOR}.${_mtkx_PATCH}
  DESCRIPTION "C99 memory-management toolkit for allocators, GC experiments and runtime integrations"
  LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(MemTKX_BUILD_TESTS "Build the MemTKX test suite" ON)
option(MemTKX_BUILD_BENCHMARKS "Build the MemTKX benchmarks" OFF)
option(MemTKX_BUILD_EXAMPLES "Build the example programs" OFF)
option(MemTKX_ENABLE_WARNINGS "Compile MemTKX and its tests with strict warnings" ON)
option(MemTKX_BUILD_CXX_WRAPPER "Provide (and install) the header-only C++ wrapper" ON)
option(MemTKX_INSTALL "Generate the install target" ${PROJECT_IS_TOP_LEVEL})

if(MemTKX_BUILD_CXX_WRAPPER)
  enable_language(CXX)
endif()

# ---------------------------------------------------------------------------
#  Library
# ---------------------------------------------------------------------------

set(MemTKX_SOURCES
  src/core/base.c
  src/core/error.c
  src/core/vec.c
  src/internal/addr_map.c
  src/internal/platform.c
  src/internal/strbuf.c
  src/dsl/ast.c
  src/dsl/expr.c
  src/dsl/features.c
  src/dsl/literals.c
  src/dsl/match.c
  src/dsl/memo.c
  src/dsl/parser.c
  src/dsl/peg.c
  src/dsl/pipeline.c
  src/dsl/task.c
  src/space/space.c
  src/space/bump_pointer.c
  src/space/free_list.c
  src/space/immix_space.c
  src/space/los_space.c
  src/barrier/barrier.c
  src/scheduler/scheduler.c
  src/scheduler/coordinator.c
  src/plan/plan.c
  src/plan/marksweep.c
  src/plan/semispace.c
  src/plan/immix.c
  src/plan/generational.c
)

add_library(MemTKX ${MemTKX_SOURCES})
add_library(MemTKX::MemTKX ALIAS MemTKX)

set_target_properties(MemTKX PROPERTIES
  OUTPUT_NAME memtkx
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME MemTKX
)

target_include_directories(MemTKX
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(MemTKX PUBLIC c_std_99)
target_compile_definitions(MemTKX PRIVATE MTKX_BUILDING_LIBRARY)
if(BUILD_SHARED_LIBS)
  target_compile_definitions(MemTKX PUBLIC MTKX_SHARED)
endif()
if(NOT WIN32)
  target_compile_definitions(MemTKX PRIVATE _POSIX_C_SOURCE=200809L)
endif()

set(MemTKX_WARNING_FLAGS
  $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes>
  $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow>
  $<$<C_COMPILER_ID:MSVC>:/W4>
)
if(MemTKX_ENABLE_WARNINGS)
  target_compile_options(MemTKX PRIVATE ${MemTKX_WARNING_FLAGS})
endif()

find_package(Threads REQUIRED)
target_link_libraries(MemTKX PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
#  Header-only C++ wrapper
# ---------------------------------------------------------------------------

if(MemTKX_BUILD_CXX_WRAPPER)
  add_library(MemTKXCpp INTERFACE)
  add_library(MemTKX::MemTKXCpp ALIAS MemTKXCpp)
  set_target_properties(MemTKXCpp PROPERTIES EXPORT_NAME MemTKXCpp)
  target_link_libraries(MemTKXCpp INTERFACE MemTKX::MemTKX)
  target_compile_features(MemTKXCpp INTERFACE cxx_std_17)
endif()

# ---------------------------------------------------------------------------
#  Tests, benchmarks, examples
# ---------------------------------------------------------------------------

if(MemTKX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MemTKX_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(MemTKX_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
#  Installation and package export
# ---------------------------------------------------------------------------

if(MemTKX_INSTALL)
  set(MemTKX_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/MemTKX)

  install(DIRECTORY include/memtkx
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
          FILES_MATCHING PATTERN "*.h")

  set(MemTKX_EXPORTED_TARGETS MemTKX)
  if(MemTKX_BUILD_CXX_WRAPPER)
    install(DIRECTORY include/memtkx
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.hpp")
    list(APPEND MemTKX_EXPORTED_TARGETS MemTKXCpp)
  endif()

  install(TARGETS ${MemTKX_EXPORTED_TARGETS}
          EXPORT MemTKXTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(EXPORT MemTKXTargets
          FILE MemTKXTargets.cmake
          NAMESPACE MemTKX::
          DESTINATION ${MemTKX_CMAKE_DIR})

  configure_package_config_file(
    cmake/MemTKXConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/MemTKXConfig.cmake"
    INSTALL_DESTINATION ${MemTKX_CMAKE_DIR})

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MemTKXConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

  install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/MemTKXConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/MemTKXConfigVersion.cmake"
          DESTINATION ${MemTKX_CMAKE_DIR})

  if(BUILD_SHARED_LIBS)
    set(MTKX_PC_CFLAGS "-DMTKX_SHARED")
  else()
    set(MTKX_PC_CFLAGS "")
  endif()
  configure_file(cmake/memtkx.pc.in "${CMAKE_CURRENT_BINARY_DIR}/memtkx.pc" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/memtkx.pc"
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

  install(FILES README.md GUIDE.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
