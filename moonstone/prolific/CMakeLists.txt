foreach(required IN ITEMS lmdbxx/lmdb++.h lua/lua.h kaguya/include/kaguya/kaguya.hpp)
  if(NOT EXISTS "${MOONSTONE_VENDOR_ROOT}/${required}")
    message(FATAL_ERROR "Prolific requires vendored ${required} under MOONSTONE_VENDOR_ROOT")
  endif()
endforeach()
find_path(PROLIFIC_LMDB_INCLUDE_DIR lmdb.h REQUIRED)
find_library(PROLIFIC_LMDB_LIBRARY NAMES lmdb REQUIRED)
find_package(Threads REQUIRED)

# Build the inspected vendored Lua, not a system Lua with a potentially different ABI.
set(lua_sources lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject
  lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib
  ldblib liolib lmathlib loslib ltablib lstrlib lutf8lib loadlib lcorolib linit)
list(TRANSFORM lua_sources PREPEND "${MOONSTONE_VENDOR_ROOT}/lua/")
list(TRANSFORM lua_sources APPEND ".c")
add_library(moonstone_prolific_lua_vendor STATIC ${lua_sources})
set_target_properties(moonstone_prolific_lua_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON
  C_STANDARD 99 C_STANDARD_REQUIRED YES C_VISIBILITY_PRESET hidden)
target_include_directories(moonstone_prolific_lua_vendor SYSTEM PUBLIC "${MOONSTONE_VENDOR_ROOT}/lua")
if(UNIX)
  target_compile_definitions(moonstone_prolific_lua_vendor PRIVATE LUA_USE_POSIX)
  target_link_libraries(moonstone_prolific_lua_vendor PUBLIC m ${CMAKE_DL_LIBS})
endif()

add_library(moonstone_prolific SHARED Prolific.cpp Codec.cpp Cache.cpp CAPI.cpp)
add_library(Moonstone::Prolific ALIAS moonstone_prolific)
target_link_libraries(moonstone_prolific PUBLIC moonstone_core PRIVATE Threads::Threads "${PROLIFIC_LMDB_LIBRARY}")
target_include_directories(moonstone_prolific SYSTEM PRIVATE
  "${MOONSTONE_VENDOR_ROOT}/lmdbxx" "${PROLIFIC_LMDB_INCLUDE_DIR}")
target_compile_definitions(moonstone_prolific PUBLIC PROLIFIC_SHARED PRIVATE PROLIFIC_BUILDING)
set_target_properties(moonstone_prolific PROPERTIES VERSION 1.0.0 SOVERSION 1 WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_library(moonstone_prolific_lua SHARED LuaProfiler.cpp)
add_library(Moonstone::ProlificLua ALIAS moonstone_prolific_lua)
target_link_libraries(moonstone_prolific_lua PUBLIC moonstone_prolific PRIVATE moonstone_prolific_lua_vendor)
target_include_directories(moonstone_prolific_lua SYSTEM PRIVATE "${MOONSTONE_VENDOR_ROOT}/kaguya/include")
set_target_properties(moonstone_prolific_lua PROPERTIES VERSION 1.0.0 SOVERSION 1
  WINDOWS_EXPORT_ALL_SYMBOLS ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)

set(prolific_targets moonstone_prolific moonstone_prolific_lua moonstone_prolific_lua_vendor)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_library(moonstone_prolific_inject SHARED Inject.cpp)
  target_link_libraries(moonstone_prolific_inject PRIVATE moonstone_prolific)
  list(APPEND prolific_targets moonstone_prolific_inject)
endif()
foreach(target IN LISTS prolific_targets)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU>:-fno-instrument-functions>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fno-instrument-functions>)
  # Clang lacks GCC's disabling flag and requires balanced attribute pragmas.
  # Wrap complete translation units, including vendor code/template instances,
  # without touching vendors or suppressing unmatched-pragma diagnostics.
  if(CMAKE_C_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    get_target_property(original_sources ${target} SOURCES)
    set(wrapped_sources)
    foreach(source IN LISTS original_sources)
      get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
      get_filename_component(extension "${source}" EXT)
      string(MAKE_C_IDENTIFIER "${source}" id)
      set(wrapper "${CMAKE_CURRENT_BINARY_DIR}/uninstrumented/${target}/${id}${extension}")
      file(GENERATE OUTPUT "${wrapper}" CONTENT
        "#ifdef __clang__\n#pragma clang attribute push(__attribute__((no_instrument_function)), apply_to=function)\n#endif\n#include \"${absolute}\"\n#ifdef __clang__\n#pragma clang attribute pop\n#endif\n")
      list(APPEND wrapped_sources "${wrapper}")
    endforeach()
    set_property(TARGET ${target} PROPERTY SOURCES "${wrapped_sources}")
  endif()
  if(NOT target STREQUAL "moonstone_prolific_lua_vendor")
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endif()
endforeach()

if(MOONSTONE_BUILD_TOOLS)
  add_executable(prolific tools/prolific.cpp)
  target_link_libraries(prolific PRIVATE moonstone_prolific_lua)
  add_executable(prolific-host examples/host.cpp)
  target_link_libraries(prolific-host PRIVATE moonstone_prolific_lua)
  install(TARGETS prolific RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  foreach(target IN ITEMS moonstone_prolific moonstone_prolific_lua moonstone_prolific_inject)
    if(TARGET ${target})
      set_property(TARGET ${target} PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()
  endforeach()
  if(TARGET prolific)
    if(IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}" OR IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
      set_property(TARGET prolific PROPERTY INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")
    else()
      file(RELATIVE_PATH library_from_bin "/${CMAKE_INSTALL_BINDIR}" "/${CMAKE_INSTALL_LIBDIR}")
      set_property(TARGET prolific PROPERTY INSTALL_RPATH "$ORIGIN/${library_from_bin}")
    endif()
  endif()
endif()
install(TARGETS moonstone_prolific moonstone_prolific_lua
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET moonstone_prolific_inject)
  install(TARGETS moonstone_prolific_inject LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(FILES Prolific.hpp Cache.hpp LuaProfiler.hpp prolific.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/moonstone/prolific)

if(MOONSTONE_BUILD_TESTS)
  foreach(feature IN ITEMS recorder cache lua)
    moonstone_test(prolific_${feature} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${feature}.cpp" moonstone_prolific_lua)
    set_tests_properties(moonstone.prolific_${feature} PROPERTIES TIMEOUT 30)
  endforeach()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The test host exports a conflicting Lua API entry point. The private
    # vendored VM must not accidentally bind to an embedding host's Lua ABI.
    set_property(TARGET moonstone_test_prolific_lua PROPERTY ENABLE_EXPORTS ON)
  endif()
  moonstone_test(prolific_c_api "${CMAKE_CURRENT_SOURCE_DIR}/tests/c_api.c" moonstone_prolific)
  set_target_properties(moonstone_test_prolific_c_api PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  set(header_sources)
  foreach(header IN ITEMS Prolific.hpp Cache.hpp LuaProfiler.hpp prolific.h)
    string(MAKE_C_IDENTIFIER "${header}" id)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header-${id}.cpp")
    file(GENERATE OUTPUT "${source}" CONTENT "#include \"prolific/${header}\"\n")
    list(APPEND header_sources "${source}")
  endforeach()
  add_library(moonstone_prolific_header_checks OBJECT ${header_sources})
  target_link_libraries(moonstone_prolific_header_checks PRIVATE moonstone_prolific)
  if(MSVC)
    target_compile_options(moonstone_prolific_header_checks PRIVATE /W4 /WX)
  else()
    target_compile_options(moonstone_prolific_header_checks PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  if(TARGET moonstone_prolific_inject)
    add_executable(prolific-instrumented-host tests/instrumented.c)
    target_link_libraries(prolific-instrumented-host PRIVATE Threads::Threads)
    target_compile_options(prolific-instrumented-host PRIVATE -finstrument-functions)
    add_executable(prolific-check-injection tests/check_injection.cpp)
    target_link_libraries(prolific-check-injection PRIVATE moonstone_prolific)
    add_test(NAME moonstone.prolific_injection COMMAND "${CMAKE_COMMAND}"
      "-DHOST=$<TARGET_FILE:prolific-instrumented-host>"
      "-DINJECT=$<TARGET_FILE:moonstone_prolific_inject>"
      "-DCHECK=$<TARGET_FILE:prolific-check-injection>"
      "-DCACHE=${CMAKE_CURRENT_BINARY_DIR}/injection-cache"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/injection.cmake")
    set_tests_properties(moonstone.prolific_injection PROPERTIES TIMEOUT 30)
  endif()
  if(MOONSTONE_BUILD_TOOLS)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME moonstone.prolific_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.py" "$<TARGET_FILE:prolific>"
        "$<TARGET_FILE:prolific-host>" "${CMAKE_CURRENT_BINARY_DIR}")
      set_tests_properties(moonstone.prolific_cli PROPERTIES TIMEOUT 30)
    endif()
  endif()
endif()
