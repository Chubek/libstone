cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(MoonstoneExotract VERSION 0.2.0 LANGUAGES C CXX)
endif()
include(GNUInstallDirs)
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME Exotract)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(_exotract_default_vendor "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party" ABSOLUTE)
if(DEFINED MOONSTONE_VENDOR_ROOT)
  set(EXOTRACT_VENDOR_ROOT "${MOONSTONE_VENDOR_ROOT}" CACHE PATH "LibStone vendor root")
else()
  set(EXOTRACT_VENDOR_ROOT "${_exotract_default_vendor}" CACHE PATH "LibStone vendor root")
endif()
option(EXOTRACT_ENABLE_LUA "Enable vendored Lua/Kaguya API" ON)
option(EXOTRACT_ENABLE_LMDB "Enable persistent LMDB storage" ON)
option(EXOTRACT_ENABLE_UCPP "Build the vendored ucpp preprocessor" ON)
option(EXOTRACT_ENABLE_CPARSER "Build isolated cparser-iso11 probe" OFF)
option(EXOTRACT_ENABLE_EXOML "Link directly to Moonstone::ExoMLExtract when available" ON)
option(EXOTRACT_BUILD_TESTS "Build Exotract tests" ON)
set(EXOTRACT_MULIB_ROOT "" CACHE PATH "Pinned canonical MuLIB checkout for integration tests")
set(EXOTRACT_MULIB_REVISION "" CACHE STRING "Expected canonical MuLIB commit for integration tests")

add_library(moonstone_exotract STATIC src/Util.cpp src/Store.cpp src/Engine.cpp)
add_library(Moonstone::Exotract ALIAS moonstone_exotract)
set_target_properties(moonstone_exotract PROPERTIES EXPORT_NAME Exotract)
target_compile_features(moonstone_exotract PUBLIC cxx_std_20)
if(EXOTRACT_ENABLE_EXOML AND NOT TARGET Moonstone::ExoMLExtract)
  find_package(MoonstoneExoMLExtract QUIET CONFIG)
endif()
target_include_directories(moonstone_exotract PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(NOT MSVC)
  target_compile_options(moonstone_exotract PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(EXOTRACT_ENABLE_EXOML AND TARGET Moonstone::ExoMLExtract)
  target_link_libraries(moonstone_exotract PUBLIC Moonstone::ExoMLExtract)
  target_compile_definitions(moonstone_exotract PRIVATE EXOTRACT_WITH_EXOML=1)
  message(STATUS "Exotract: typed discovery uses linked ExoML")
endif()

if(EXOTRACT_ENABLE_LMDB)
  find_path(EXOTRACT_LMDB_INCLUDE lmdb.h QUIET)
  find_library(EXOTRACT_LMDB_LIB lmdb QUIET)
  if(EXISTS "${EXOTRACT_VENDOR_ROOT}/lmdbxx/lmdb++.h" AND EXOTRACT_LMDB_INCLUDE AND EXOTRACT_LMDB_LIB)
    target_include_directories(moonstone_exotract SYSTEM PRIVATE
      "${EXOTRACT_VENDOR_ROOT}/lmdbxx" "${EXOTRACT_LMDB_INCLUDE}")
    target_link_libraries(moonstone_exotract PRIVATE "${EXOTRACT_LMDB_LIB}")
    target_compile_definitions(moonstone_exotract PRIVATE EXOTRACT_WITH_LMDB=1)
    set(_exotract_has_lmdb ON)
    message(STATUS "Exotract: persistent LMDB enabled")
  else()
    message(STATUS "Exotract: LMDB unavailable; persistent cache requests fail explicitly")
  endif()
endif()

if(EXOTRACT_ENABLE_UCPP AND EXISTS "${EXOTRACT_VENDOR_ROOT}/ucpp/cpp.c")
  set(ucpp_root "${EXOTRACT_VENDOR_ROOT}/ucpp")
  add_executable(exotract-ucpp
    "${ucpp_root}/assert.c" "${ucpp_root}/cpp.c"
    "${ucpp_root}/eval.c" "${ucpp_root}/lexer.c"
    "${ucpp_root}/macro.c" "${ucpp_root}/mem.c" "${ucpp_root}/nhash.c")
  target_compile_definitions(exotract-ucpp PRIVATE STAND_ALONE UCPP_CONFIG)
  target_include_directories(exotract-ucpp PRIVATE "${ucpp_root}")
  set_target_properties(exotract-ucpp PROPERTIES C_STANDARD 99)
  target_compile_definitions(moonstone_exotract PRIVATE
    EXOTRACT_UCPP_EXECUTABLE="$<TARGET_FILE:exotract-ucpp>")
  add_dependencies(moonstone_exotract exotract-ucpp)
endif()

if(EXOTRACT_ENABLE_CPARSER AND EXISTS "${EXOTRACT_VENDOR_ROOT}/cparser-iso11/src/CParser.cpp")
  set(cparser_root "${EXOTRACT_VENDOR_ROOT}/cparser-iso11")
  file(GLOB cparser_sources CONFIGURE_DEPENDS "${cparser_root}/src/*.cpp")
  list(FILTER cparser_sources EXCLUDE REGEX "/(cformat|test[^/]*|main)\\.cpp$")
  add_executable(exotract-cparser-probe tools/cparser-probe.cpp ${cparser_sources})
  target_include_directories(exotract-cparser-probe PRIVATE "${cparser_root}/include")
  target_precompile_headers(exotract-cparser-probe PRIVATE <cstdarg> <cstdio> <cstdlib>)
  target_compile_definitions(moonstone_exotract PRIVATE
    EXOTRACT_CPARSER_EXECUTABLE="$<TARGET_FILE:exotract-cparser-probe>")
  add_dependencies(moonstone_exotract exotract-cparser-probe)
endif()

add_executable(exotract tools/exotract.cpp)
target_link_libraries(exotract PRIVATE moonstone_exotract)

if(EXOTRACT_ENABLE_LUA AND EXISTS "${EXOTRACT_VENDOR_ROOT}/lua/lua.h" AND
    EXISTS "${EXOTRACT_VENDOR_ROOT}/kaguya/include/kaguya/kaguya.hpp")
  set(lua_root "${EXOTRACT_VENDOR_ROOT}/lua")
  set(lua_sources lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject
    lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib
    ldblib liolib lmathlib loslib ltablib lstrlib lutf8lib loadlib lcorolib linit)
  list(TRANSFORM lua_sources PREPEND "${lua_root}/")
  list(TRANSFORM lua_sources APPEND ".c")
  # Lua's C++ exception backend unwinds C++ objects on Lua errors. Public
  # declarations retain their C ABI; vendor files themselves remain untouched.
  set(lua_cxx_sources)
  foreach(source IN LISTS lua_sources)
    get_filename_component(stem "${source}" NAME_WE)
    set(wrapper "${CMAKE_CURRENT_BINARY_DIR}/lua-cxx/${stem}.cpp")
    file(GENERATE OUTPUT "${wrapper}" CONTENT
      "#include \"${lua_root}/lprefix.h\"\nextern \"C\" {\n#include \"${lua_root}/lua.h\"\n#include \"${lua_root}/lauxlib.h\"\n#include \"${lua_root}/lualib.h\"\n}\n#include \"${source}\"\n")
    list(APPEND lua_cxx_sources "${wrapper}")
  endforeach()
  add_library(exotract_lua_vendor STATIC ${lua_cxx_sources})
  set_target_properties(exotract_lua_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
  target_include_directories(exotract_lua_vendor PRIVATE "${lua_root}")
  if(UNIX)
    target_compile_definitions(exotract_lua_vendor PRIVATE LUA_USE_POSIX)
    target_link_libraries(exotract_lua_vendor PUBLIC m ${CMAKE_DL_LIBS})
  endif()
  add_library(moonstone_exotract_lua STATIC src/LuaRuntime.cpp)
  add_library(Moonstone::ExotractLua ALIAS moonstone_exotract_lua)
  set_target_properties(moonstone_exotract_lua PROPERTIES EXPORT_NAME ExotractLua)
  target_include_directories(moonstone_exotract_lua PRIVATE
    "${EXOTRACT_VENDOR_ROOT}/kaguya/include" "${lua_root}")
  target_link_libraries(moonstone_exotract_lua PUBLIC moonstone_exotract PRIVATE exotract_lua_vendor)
  target_compile_definitions(exotract PRIVATE EXOTRACT_WITH_LUA=1)
  target_link_libraries(exotract PRIVATE moonstone_exotract_lua)
  if(NOT MSVC)
    target_compile_options(moonstone_exotract_lua PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  install(TARGETS moonstone_exotract_lua exotract_lua_vendor EXPORT MoonstoneExotractTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  message(STATUS "Exotract: vendored Lua/Kaguya enabled")
else()
  message(STATUS "Exotract: Lua/Kaguya unavailable; native CLI remains usable")
endif()

install(TARGETS moonstone_exotract exotract EXPORT MoonstoneExotractTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/exotract DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT MoonstoneExotractTargets NAMESPACE Moonstone::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonstoneExotract)
include(CMakePackageConfigHelpers)
set(EXOTRACT_PACKAGE_EXOML OFF)
if(EXOTRACT_ENABLE_EXOML AND TARGET Moonstone::ExoMLExtract)
  set(EXOTRACT_PACKAGE_EXOML ON)
endif()
configure_package_config_file(cmake/MoonstoneExotractConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/MoonstoneExotractConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonstoneExotract)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/MoonstoneExotractConfig.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonstoneExotract)
install(DIRECTORY lua/ DESTINATION ${CMAKE_INSTALL_DATADIR}/moonstone/exotract/lua)
install(DIRECTORY binders/ DESTINATION ${CMAKE_INSTALL_DATADIR}/moonstone/exotract/binders)
install(DIRECTORY contracts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/moonstone/exotract/contracts)
if(EXOTRACT_BUILD_TESTS)
  enable_testing()
  set(EXOTRACT_TEST_RESOURCE_DIR "" CACHE PATH "Clang resource directory for discovery tests")
  set(EXOTRACT_TEST_EXOML_BIND "" CACHE FILEPATH "Installed exoml-bind for standalone discovery tests")
  set(_exotract_test_bind "${EXOTRACT_TEST_EXOML_BIND}")
  if(TARGET exoml-bind)
    set(_exotract_test_bind "$<TARGET_FILE:exoml-bind>")
  endif()
  if((EXOTRACT_PACKAGE_EXOML OR _exotract_test_bind) AND NOT EXOTRACT_TEST_RESOURCE_DIR)
    find_program(EXOTRACT_TEST_CLANG NAMES clang REQUIRED)
    execute_process(COMMAND "${EXOTRACT_TEST_CLANG}" -print-resource-dir
      OUTPUT_VARIABLE EXOTRACT_TEST_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY)
  endif()
  add_executable(exotract_tests tests/test_core.cpp)
  target_link_libraries(exotract_tests PRIVATE moonstone_exotract)
  if(NOT MSVC)
    target_compile_options(exotract_tests PRIVATE -UNDEBUG)
  endif()
  target_compile_definitions(exotract_tests PRIVATE EXOTRACT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.h"
    EXOTRACT_TEST_COMPILER="${CMAKE_C_COMPILER}"
    EXOTRACT_TEST_RESOURCE_DIR="${EXOTRACT_TEST_RESOURCE_DIR}")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/header-check.cpp"
    CONTENT "#include <exotract/Exotract.hpp>\n")
  add_library(exotract_header_check OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header-check.cpp")
  target_link_libraries(exotract_header_check PRIVATE moonstone_exotract)
  if(NOT MSVC)
    target_compile_options(exotract_header_check PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME exotract_core COMMAND exotract_tests)
  if(UNIX)
    find_package(Threads REQUIRED)
    add_executable(exotract_hardening tests/hardening.cpp)
    target_link_libraries(exotract_hardening PRIVATE moonstone_exotract Threads::Threads)
    target_compile_options(exotract_hardening PRIVATE -UNDEBUG)
    target_compile_definitions(exotract_hardening PRIVATE EXOTRACT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.h")
    add_test(NAME exotract_hardening COMMAND exotract_hardening)
  endif()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME exotract_import
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/import_cli.py"
        "$<TARGET_FILE:exotract>" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.h" "${EXOTRACT_TEST_RESOURCE_DIR}" "${CMAKE_C_COMPILER}")
    add_test(NAME exotract_integration
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py"
       "$<TARGET_FILE:exotract>" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.h" "${EXOTRACT_TEST_RESOURCE_DIR}" "${CMAKE_C_COMPILER}")
    add_test(NAME exotract_negative COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative.py" "$<TARGET_FILE:exotract>"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.h" "${EXOTRACT_TEST_RESOURCE_DIR}" "${CMAKE_C_COMPILER}")
    if(_exotract_has_lmdb AND TARGET exotract_hardening)
      add_test(NAME exotract_store COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/store.py" "$<TARGET_FILE:exotract_hardening>")
    endif()
    if(_exotract_test_bind)
      add_test(NAME exotract_discovery COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery.py" "$<TARGET_FILE:exotract>"
        "${_exotract_test_bind}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.h"
        "${EXOTRACT_TEST_RESOURCE_DIR}" "$<BOOL:${EXOTRACT_PACKAGE_EXOML}>"
        "$<BOOL:${_exotract_has_lmdb}>" "${CMAKE_C_COMPILER}")
      set_tests_properties(exotract_discovery PROPERTIES TIMEOUT 180)
    endif()
    if(TARGET exotract-ucpp)
      add_test(NAME exotract_ucpp COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/probes.py" ucpp "$<TARGET_FILE:exotract-ucpp>"
        "$<TARGET_FILE:exotract>" "${EXOTRACT_TEST_RESOURCE_DIR}" "${CMAKE_C_COMPILER}")
    endif()
    if(TARGET exotract-cparser-probe)
      add_test(NAME exotract_cparser COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/probes.py" cparser "$<TARGET_FILE:exotract-cparser-probe>"
        "$<TARGET_FILE:exotract>" "${EXOTRACT_TEST_RESOURCE_DIR}" "${CMAKE_C_COMPILER}")
    endif()
    if(TARGET moonstone_exotract_lua)
      add_test(NAME exotract_lua COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua.py" "$<TARGET_FILE:exotract>"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.h" "${_exotract_test_bind}" "${EXOTRACT_TEST_RESOURCE_DIR}" "${CMAKE_C_COMPILER}")
    endif()
    if(EXOTRACT_MULIB_ROOT)
      if(NOT EXOTRACT_MULIB_REVISION OR NOT _exotract_test_bind)
        message(FATAL_ERROR "MicroJS tests require EXOTRACT_MULIB_REVISION and exoml-bind")
      endif()
      add_test(NAME exotract_microjs COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/microjs.py" "$<TARGET_FILE:exotract>"
        "${_exotract_test_bind}" "${EXOTRACT_TEST_RESOURCE_DIR}"
        "${EXOTRACT_MULIB_ROOT}" "${EXOTRACT_MULIB_REVISION}" "${CMAKE_C_COMPILER}"
        "${CMAKE_C_FLAGS}" "$<AND:$<TARGET_EXISTS:moonstone_exotract_lua>,$<BOOL:${_exotract_has_lmdb}>>")
      set_tests_properties(exotract_microjs PROPERTIES TIMEOUT 180)
    endif()
  endif()
  add_test(NAME exotract_cli COMMAND exotract inspect --header
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.h" --name Fixture --resource-dir "${EXOTRACT_TEST_RESOURCE_DIR}" --compiler "${CMAKE_C_COMPILER}")
  add_test(NAME exotract_package COMMAND "${CMAKE_COMMAND}"
    "-DBUILD=${CMAKE_BINARY_DIR}" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/package-test"
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer" "-DCXX=${CMAKE_CXX_COMPILER}" "-DCC=${CMAKE_C_COMPILER}"
    "-DFLAGS=${CMAKE_CXX_FLAGS}" "-DCONFIG=$<CONFIG>" "-DEXTRACT=${EXOTRACT_PACKAGE_EXOML}"
    "-DPREFIX=${CMAKE_PREFIX_PATH}" "-DRESOURCES=${EXOTRACT_TEST_RESOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package.cmake")
  set_tests_properties(exotract_package PROPERTIES TIMEOUT 90)
endif()
