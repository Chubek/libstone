function(asdl_add_vendored_lua)
  set(lua_sources)
  foreach(unit IN ITEMS lapi lauxlib lbaselib lcode lcorolib lctype ldblib
      ldebug ldo ldump lfunc lgc linit liolib llex lmathlib lmem loadlib
      lobject lopcodes loslib lparser lstate lstring lstrlib ltable ltablib
      ltm lundump lutf8lib lvm lzio)
    list(APPEND lua_sources "${ASDL_THIRD_PARTY_DIR}/lua/${unit}.c")
  endforeach()

  # Lua uses native exception handling in C++ builds. The forced header keeps
  # its public symbols at C linkage for consumers of the native loader.
  set_source_files_properties(${lua_sources} PROPERTIES LANGUAGE CXX)
  add_library(asdl_lua_vendor OBJECT ${lua_sources})
  target_compile_features(asdl_lua_vendor PRIVATE cxx_std_17)
  target_include_directories(asdl_lua_vendor SYSTEM PUBLIC "${ASDL_THIRD_PARTY_DIR}/lua")
  set_target_properties(asdl_lua_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)
  set(lua_abi_header "${CMAKE_CURRENT_SOURCE_DIR}/src/lua_vendor.hpp")
  if(MSVC)
    target_compile_options(asdl_lua_vendor PRIVATE "/FI${lua_abi_header}")
  else()
    target_compile_options(asdl_lua_vendor PRIVATE "-include" "${lua_abi_header}")
  endif()
  if(UNIX)
    # Native Lua module lookup is managed by the ASDL loader; do not compile
    # the vendored library with dynamic Lua package loading enabled.
    target_compile_definitions(asdl_lua_vendor PRIVATE LUA_USE_POSIX)
  endif()
endfunction()
