# One bundled Lua ABI for Sandstone and Yellowstone, including superbuilds.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/LibstoneBuild.cmake")
libstone_require_files(LIBSTONE_LUA_SOURCE_DIR lua.h lapi.c)
libstone_require_files(LIBSTONE_KAGUYA_SOURCE_DIR include/kaguya/kaguya.hpp)
set(_lua_sources lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c
  ldblib.c ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c
  lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c
  lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c
  lutf8lib.c lvm.c lzio.c)
list(TRANSFORM _lua_sources PREPEND "${LIBSTONE_LUA_SOURCE_DIR}/")
add_library(libstone_script_lua STATIC ${_lua_sources})
set_target_properties(libstone_script_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(libstone_script_lua SYSTEM PUBLIC
  $<BUILD_INTERFACE:${LIBSTONE_LUA_SOURCE_DIR}>)
if(UNIX)
  target_compile_definitions(libstone_script_lua PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
  target_link_libraries(libstone_script_lua PUBLIC m ${CMAKE_DL_LIBS})
endif()
add_library(libstone_kaguya INTERFACE)
target_include_directories(libstone_kaguya SYSTEM INTERFACE
  $<BUILD_INTERFACE:${LIBSTONE_KAGUYA_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>)
target_link_libraries(libstone_kaguya INTERFACE libstone_script_lua)
install(TARGETS libstone_script_lua ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
