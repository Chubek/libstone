cmake_minimum_required(VERSION 3.20)
project(Flintstone VERSION 0.1.0 LANGUAGES C CXX)
include(../cmake/LibstoneBuild.cmake)
include(CMakePackageConfigHelpers)
option(FLINTSTONE_BUILD_TESTS "Build Flintstone contract and integration tests" "${LIBSTONE_BUILD_TESTS}")
option(FLINTSTONE_BUILD_TOOLS "Build the Flintstone IL tool" "${LIBSTONE_BUILD_TOOLS}")
option(FLINTSTONE_BUILD_BINDING_TESTS "Validate the Tourchstone XBind specification" "${FLINTSTONE_BUILD_TESTS}")

# Standalone builds use the same libraries as aggregate builds. Never fetch or
# search for machine-specific snapshots. Only default options we own as a host.
if(NOT TARGET Firestone::ir)
  if(NOT DEFINED FIRESTONE_BUILD_TESTS)
    set(FIRESTONE_BUILD_TESTS OFF CACHE BOOL "Build Firestone tests")
  endif()
  add_subdirectory(../firestone dependencies/firestone)
endif()
if(NOT TARGET Blackstone::blackstone)
  if(NOT DEFINED BLACKSTONE_BUILD_TESTS)
    set(BLACKSTONE_BUILD_TESTS OFF CACHE BOOL "Build Blackstone tests")
  endif()
  if(NOT DEFINED BLACKSTONE_BUILD_TOOLS)
    set(BLACKSTONE_BUILD_TOOLS OFF CACHE BOOL "Build Blackstone tools")
  endif()
  add_subdirectory(../blackstone dependencies/blackstone)
endif()
if(NOT TARGET sandstone)
  foreach(option IN ITEMS SANDSTONE_BUILD_TESTS SANDSTONE_BUILD_TOOLS
      SANDSTONE_BUILD_COMPILER SANDSTONE_BUILD_ASSEMBLER SANDSTONE_BUILD_SANDSLAB)
    if(NOT DEFINED ${option})
      set(${option} OFF CACHE BOOL "Optional standalone Sandstone facilities")
    endif()
  endforeach()
  add_subdirectory(../sandstone dependencies/sandstone)
endif()
if(NOT TARGET XBind::xbind)
  foreach(option IN ITEMS XBIND_BUILD_TESTS XBIND_BUILD_QAMRPP_BINDING)
    if(NOT DEFINED ${option})
      set(${option} OFF CACHE BOOL "Optional standalone XBind facilities")
    endif()
  endforeach()
  add_subdirectory(../tourchstone/xbind dependencies/xbind)
endif()

if(TARGET schedrow AND TARGET regtl)
  set(flintstone_limestone_libraries schedrow regtl)
  # These static libraries are also embedded in the shared C ABI.
  foreach(target IN ITEMS schedrow regtl metacode limestone_parser_tables limestone_dparse)
    if(TARGET ${target})
      set_property(TARGET ${target} PROPERTY POSITION_INDEPENDENT_CODE ON)
    endif()
  endforeach()
else()
  # Reuse Limestone's actual algorithms, not copies/fallback implementations.
  # Their narrow source boundary does not need the unrelated Tunah/SAT/parser/FFI
  # dependencies or the full Limestone orchestration library.
  add_library(flintstone_limestone_services STATIC
    ../limestone/schedrow/schedrow.cpp ../limestone/schedrow/cfg.cpp
    ../limestone/schedrow/groups.cpp ../limestone/schedrow/timing.cpp
    ../limestone/regtl/regtl.cpp ../limestone/regtl/analysis.cpp
    ../limestone/regtl/coloring.cpp ../limestone/regtl/pbqp.cpp)
  target_compile_features(flintstone_limestone_services PUBLIC cxx_std_20)
  set_target_properties(flintstone_limestone_services PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF)
  set(flintstone_limestone_libraries flintstone_limestone_services)
endif()

add_library(flintstone SHARED Flintstone.cpp Flinestone.c ir.cpp
  dialects/builtin.cpp dialects/vector.cpp dialects/tuple.cpp dialects/ptr.cpp
  frontend/parser.cpp frontend/semantic_analysis.cpp frontend/imports.cpp
  pass/pipeline.cpp pass/scalarize.cpp
  targets/firestone.cpp targets/native.cpp targets/sandstone_targets.cpp)
add_library(Flintstone::flintstone ALIAS flintstone)
target_compile_features(flintstone PUBLIC cxx_std_20)
target_compile_definitions(flintstone PRIVATE FLINTSTONE_STDLIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/stdlib")
target_include_directories(flintstone PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/flintstone>)
target_include_directories(flintstone SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../limestone")
target_link_libraries(flintstone PUBLIC Firestone::ir Firestone::vm
  PRIVATE Blackstone::blackstone sandstone XBind::xbind ${flintstone_limestone_libraries} ${CMAKE_DL_LIBS})
set_target_properties(flintstone PROPERTIES CXX_EXTENSIONS OFF VERSION ${PROJECT_VERSION}
  SOVERSION 0 WINDOWS_EXPORT_ALL_SYMBOLS ON)
if(UNIX AND NOT APPLE)
  set_property(TARGET flintstone PROPERTY INSTALL_RPATH "$ORIGIN")
endif()
libstone_warnings(flintstone)

add_library(flintstone_native INTERFACE)
add_library(Flintstone::native ALIAS flintstone_native)
set_target_properties(flintstone_native PROPERTIES EXPORT_NAME native)
target_include_directories(flintstone_native SYSTEM INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../limestone>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/flintstone/stone/limestone>)
target_link_libraries(flintstone_native INTERFACE flintstone
  $<BUILD_INTERFACE:sandstone> $<INSTALL_INTERFACE:Flintstone::sandstone>)

if(FLINTSTONE_BUILD_TOOLS OR FLINTSTONE_BUILD_TESTS)
  add_executable(flintstone-cli tools/main.cpp)
  set_target_properties(flintstone-cli PROPERTIES OUTPUT_NAME flintstone)
  target_link_libraries(flintstone-cli PRIVATE Flintstone::native)
  libstone_warnings(flintstone-cli)
endif()
if(FLINTSTONE_BUILD_TOOLS)
  install(TARGETS flintstone-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS flintstone flintstone_native EXPORT FlintstoneTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES Flintstone.hpp Flintstone.h Flinestone.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/flintstone)
install(FILES frontend/ast.hpp frontend/visitor.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/flintstone/frontend)
install(FILES targets/Native.hpp targets/SandstoneTargets.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/flintstone/targets)
foreach(directory IN ITEMS schedrow regtl metacode)
  if(directory STREQUAL "schedrow")
    set(headers schedrow.hpp)
  elseif(directory STREQUAL "regtl")
    set(headers regtl.hpp)
  else()
    set(headers metacode.hpp operand_constraints.hpp)
  endif()
  foreach(header IN LISTS headers)
    install(FILES "../limestone/${directory}/${header}"
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/flintstone/stone/limestone/${directory})
  endforeach()
endforeach()
install(FILES ../limestone/limestone/foundation.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/flintstone/stone/limestone/limestone)
install(FILES Flintstone.xbind DESTINATION ${CMAKE_INSTALL_DATADIR}/flintstone)
install(DIRECTORY manual examples stdlib DESTINATION ${CMAKE_INSTALL_DATADIR}/flintstone)
install(EXPORT FlintstoneTargets NAMESPACE Flintstone:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Flintstone)
set(FLINTSTONE_HAS_ZSTD OFF)
if(SANDSTONE_ZSTD_LIB AND SANDSTONE_ZSTD_INCLUDE)
  set(FLINTSTONE_HAS_ZSTD ON)
endif()
configure_package_config_file(cmake/FlintstoneConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/FlintstoneConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Flintstone)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/FlintstoneConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/FlintstoneConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/FlintstoneConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Flintstone)

if(FLINTSTONE_BUILD_TESTS)
  enable_testing()
  foreach(header IN ITEMS Flintstone.hpp frontend/ast.hpp frontend/visitor.hpp targets/Native.hpp targets/SandstoneTargets.hpp)
    string(MAKE_C_IDENTIFIER "${header}" identifier)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${identifier}.cpp")
    file(WRITE "${source}" "#include <${header}>\n")
    list(APPEND flintstone_header_sources "${source}")
  endforeach()
  add_library(flintstone-header-check OBJECT ${flintstone_header_sources})
  target_link_libraries(flintstone-header-check PRIVATE Flintstone::native)
  libstone_warnings(flintstone-header-check)
  foreach(suite IN ITEMS core native)
    add_executable(flintstone-${suite}-test tests/${suite}.cpp)
    target_link_libraries(flintstone-${suite}-test PRIVATE Flintstone::native)
    target_compile_definitions(flintstone-${suite}-test PRIVATE FLINTSTONE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
      BLACKSTONE_KERNEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../blackstone/kernels")
    libstone_warnings(flintstone-${suite}-test)
    add_test(NAME flintstone.${suite} COMMAND flintstone-${suite}-test)
    set_tests_properties(flintstone.${suite} PROPERTIES TIMEOUT 120)
  endforeach()
  add_executable(flintstone-c-test tests/c_api.c tests/c_header.c)
  target_link_libraries(flintstone-c-test PRIVATE Flintstone::flintstone)
  set_target_properties(flintstone-c-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  libstone_warnings(flintstone-c-test)
  add_test(NAME flintstone.c COMMAND flintstone-c-test)
  add_test(NAME flintstone.cli COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:flintstone-cli> -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}
    -DBINARY=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake)
  if(FLINTSTONE_BUILD_BINDING_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_dependencies(flintstone-core-test xbind_cli)
    add_test(NAME flintstone.xbind COMMAND ${CMAKE_COMMAND}
      -DXBIND=$<TARGET_FILE:xbind_cli> -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}
      -DBINARY=${CMAKE_CURRENT_BINARY_DIR} -DCXX=${CMAKE_CXX_COMPILER}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/xbind.cmake)
    set(binding_dir "${CMAKE_CURRENT_BINARY_DIR}/bindings")
    set(xbind_root "${CMAKE_CURRENT_SOURCE_DIR}/../tourchstone/xbind")
    add_custom_command(OUTPUT "${binding_dir}/flintstone_lua.cpp" "${binding_dir}/domluabind/capi.h"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${binding_dir}/domluabind"
      COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${xbind_root}/dombind/domqlib"
        ${Python3_EXECUTABLE} -B "${xbind_root}/dombind/domluabind/generate.py"
        "${binding_dir}/domluabind/capi.h"
      COMMAND ${CMAKE_COMMAND} -DXBIND=$<TARGET_FILE:xbind_cli>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -DOUTPUT=${binding_dir}/flintstone_lua.cpp
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_lua.cmake
      DEPENDS xbind_cli Flintstone.xbind tests/generate_lua.cmake
        "${xbind_root}/dombind/domluabind/generate.py"
      VERBATIM)
    add_executable(flintstone-lua-test tests/lua.cpp "${binding_dir}/flintstone_lua.cpp")
    target_include_directories(flintstone-lua-test PRIVATE "${binding_dir}" "${LIBSTONE_LUA_SOURCE_DIR}")
    target_link_libraries(flintstone-lua-test PRIVATE Flintstone::flintstone blackstone_lua)
    libstone_warnings(flintstone-lua-test)
    add_test(NAME flintstone.lua COMMAND flintstone-lua-test)
  endif()
  add_test(NAME flintstone.install COMMAND ${CMAKE_COMMAND}
    -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -DBINARY=${CMAKE_CURRENT_BINARY_DIR}
    -DPREFIX_ROOT=${CMAKE_CURRENT_BINARY_DIR} -DGENERATOR=${CMAKE_GENERATOR}
    -DCC=${CMAKE_C_COMPILER} -DCXX=${CMAKE_CXX_COMPILER}
    "-DC_FLAGS=${CMAKE_C_FLAGS}" "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
    "-DLINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/install.cmake)
  set_tests_properties(flintstone.install PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
endif()
