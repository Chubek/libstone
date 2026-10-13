cmake_minimum_required(VERSION 3.20)
project(Blackstone VERSION 0.2.0 LANGUAGES C CXX)
include(../cmake/LibstoneBuild.cmake)
include(CMakePackageConfigHelpers)
option(BLACKSTONE_BUILD_TESTS "Build Blackstone tests" "${LIBSTONE_BUILD_TESTS}")
option(BLACKSTONE_BUILD_TOOLS "Build Blackstone catalog tool" "${LIBSTONE_BUILD_TOOLS}")

# Firestone is header-only. At aggregate configuration Blackstone precedes it;
# use the headers directly and resolve the exported package for installed users.
set(BLACKSTONE_FIRESTONE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../firestone"
    CACHE PATH "Firestone header source directory")
libstone_require_files(BLACKSTONE_FIRESTONE_SOURCE_DIR libir/SimpleSSAGraph.hpp librewrite/Term.hpp)
libstone_require_files(LIBSTONE_LUA_SOURCE_DIR lua.h lauxlib.h lapi.c)
set(lua_sources)
foreach(source IN ITEMS lapi lauxlib lbaselib lcode lcorolib lctype ldblib
    ldebug ldo ldump lfunc lgc linit liolib llex lmathlib lmem loadlib
    lobject lopcodes loslib lparser lstate lstring lstrlib ltable ltablib
    ltm lundump lutf8lib lvm lzio)
  list(APPEND lua_sources "${LIBSTONE_LUA_SOURCE_DIR}/${source}.c")
endforeach()
add_library(blackstone_lua STATIC ${lua_sources})
set_target_properties(blackstone_lua PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON EXPORT_NAME lua)
target_include_directories(blackstone_lua SYSTEM PUBLIC
  $<BUILD_INTERFACE:${LIBSTONE_LUA_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/blackstone/lua>)
if(UNIX)
  target_compile_definitions(blackstone_lua PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
  target_link_libraries(blackstone_lua PRIVATE m ${CMAKE_DL_LIBS})
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS lrewrite/lrewrite.lua)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS lrewrite/local.lua)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/lrewrite/lrewrite.lua" LREWRITE_SOURCE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/lrewrite/local.lua" LOCAL_REWRITE_SOURCE)
configure_file(cmake/lrewrite_source.hpp.in lrewrite_source.hpp @ONLY)
add_library(blackstone blackstone.c lrewrite/lrewrite.cpp lua/loadlib.cpp
  lua/loadkernels.cpp lua/blackstone_lua.cpp firestone-interop/match.cpp firestone-interop/pattern.cpp)
add_library(Blackstone::blackstone ALIAS blackstone)
target_compile_features(blackstone PUBLIC cxx_std_17)
target_include_directories(blackstone PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${BLACKSTONE_FIRESTONE_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/blackstone>)
target_include_directories(blackstone PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(blackstone PRIVATE blackstone_lua)
set_target_properties(blackstone PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
libstone_warnings(blackstone)

if(BLACKSTONE_BUILD_TOOLS)
  add_executable(blackstone-kernels tools/kernels.cpp)
  target_link_libraries(blackstone-kernels PRIVATE blackstone)
  install(TARGETS blackstone-kernels RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS blackstone blackstone_lua EXPORT BlackstoneTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES blackstone.h blackstone.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/blackstone)
install(FILES firestone-interop/match.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/blackstone/firestone-interop)
install(FILES "${LIBSTONE_LUA_SOURCE_DIR}/lua.h" "${LIBSTONE_LUA_SOURCE_DIR}/luaconf.h"
  "${LIBSTONE_LUA_SOURCE_DIR}/lauxlib.h" "${LIBSTONE_LUA_SOURCE_DIR}/lualib.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/blackstone/lua)
install(DIRECTORY kernels/ DESTINATION ${CMAKE_INSTALL_DATADIR}/blackstone/kernels FILES_MATCHING PATTERN "*.lua")
install(FILES lrewrite/lrewrite.lua DESTINATION ${CMAKE_INSTALL_DATADIR}/blackstone)
install(FILES lrewrite/local.lua DESTINATION ${CMAKE_INSTALL_DATADIR}/blackstone/lrewrite)
install(EXPORT BlackstoneTargets NAMESPACE Blackstone:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Blackstone)
configure_package_config_file(cmake/BlackstoneConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/BlackstoneConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Blackstone)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/BlackstoneConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/BlackstoneConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/BlackstoneConfigVersion.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Blackstone)
if(BLACKSTONE_BUILD_TESTS)
  enable_testing()
  add_executable(blackstone-test tests/runtime.cpp tests/c_api.c)
  target_link_libraries(blackstone-test PRIVATE blackstone blackstone_lua)
  target_compile_definitions(blackstone-test PRIVATE BLACKSTONE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  libstone_warnings(blackstone-test)
  add_test(NAME blackstone.runtime COMMAND blackstone-test "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures")
  add_executable(blackstone-kernels-test tests/kernels.cpp)
  target_link_libraries(blackstone-kernels-test PRIVATE blackstone)
  target_compile_definitions(blackstone-kernels-test PRIVATE BLACKSTONE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  libstone_warnings(blackstone-kernels-test)
  add_test(NAME blackstone.kernels COMMAND blackstone-kernels-test)
  add_test(NAME blackstone.install COMMAND ${CMAKE_COMMAND}
    -DBLACKSTONE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}
    -DBLACKSTONE_BINARY=${CMAKE_CURRENT_BINARY_DIR}
    -DFIRESTONE_SOURCE=${BLACKSTONE_FIRESTONE_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/install.cmake)
  set_tests_properties(blackstone.runtime blackstone.kernels blackstone.install PROPERTIES TIMEOUT 120)
endif()
