cmake_minimum_required(VERSION 3.20)
project(Aurocks LANGUAGES C CXX)

get_filename_component(AUROCKS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${AUROCKS_ROOT}/cmake/LibstoneBuild.cmake")
set(vendor "${LIBSTONE_THIRD_PARTY_DIR}")

if(NOT TARGET chomsky3)
  set(BUILD_SHARED_LIBS OFF)
  set(CHOMSKY3_ENABLE_JIT OFF)
  set(CHOMSKY3_INSTALL OFF)
  set(CHOMSKY3_BUILD_REGEXPPC OFF)
  add_subdirectory("${vendor}/libchomsky3" vendor/chomsky3 EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET libglr)
  set(BUILD_TESTS OFF)
  set(BUILD_EXAMPLES OFF)
  set(BUILD_DOCUMENTATION OFF)
  set(EMBED_MD_DOCS OFF)
  set(BUILD_DISAMBSTD OFF)
  set(BUILD_REWRITELIB OFF)
  set(BUILD_MOOSEDOG OFF)
  set(ENABLE_CACHE OFF)
  set(INSTALL_GLRpp OFF)
  set(GLR_KLIB_DIR "${vendor}/klib")
  set(GLR_CTL_DIR "${vendor}/ctl")
  add_subdirectory("${vendor}/libglr" vendor/glr EXCLUDE_FROM_ALL)
endif()

# No interpreter executable, native loader, I/O, OS or debug library.
set(lua_sources lapi lauxlib lbaselib lcode lctype ldebug ldo ldump lfunc lgc
    llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio)
list(TRANSFORM lua_sources PREPEND "${vendor}/lua/")
list(TRANSFORM lua_sources APPEND ".c")
add_library(aurocks_lua STATIC ${lua_sources})
target_include_directories(aurocks_lua PUBLIC "${vendor}/lua")
target_compile_features(aurocks_lua PRIVATE c_std_99)
target_link_libraries(aurocks_lua PUBLIC m)
set_target_properties(aurocks_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(aurocks_compiler STATIC
  src/Compiler.cpp src/Frontend.cpp src/Backend.cpp src/Emitter.cpp
  src/LinkedEmitter.cpp src/Extensions.cpp)
add_library(Aurocks::Compiler ALIAS aurocks_compiler)
target_compile_features(aurocks_compiler PUBLIC cxx_std_20)
target_include_directories(aurocks_compiler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(aurocks_compiler SYSTEM PRIVATE
  "${AUROCKS_ROOT}" "${AUROCKS_ROOT}/firestone/liblr")
target_link_libraries(aurocks_compiler PRIVATE libglr chomsky3 aurocks_lua)
libstone_warnings(aurocks_compiler)

add_library(aurocks_runtime STATIC src/Runtime.c)
add_library(Aurocks::Runtime ALIAS aurocks_runtime)
target_compile_features(aurocks_runtime PUBLIC c_std_99 PRIVATE c_std_11)
target_include_directories(aurocks_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aurocks_runtime PUBLIC libglr)
libstone_warnings(aurocks_runtime)

if(LIBSTONE_BUILD_TOOLS)
  add_executable(aurocks CLI/main.cpp)
  target_link_libraries(aurocks PRIVATE aurocks_compiler)
  libstone_warnings(aurocks)
endif()

if(LIBSTONE_BUILD_TESTS)
  enable_testing()
  add_executable(aurocks_c_api_test tests/c_api.c)
  target_link_libraries(aurocks_c_api_test PRIVATE aurocks_compiler)
  add_test(NAME aurocks.c_api COMMAND aurocks_c_api_test)
  add_executable(aurocks_linked_test tests/linked.c)
  target_link_libraries(aurocks_linked_test PRIVATE aurocks_runtime)
  add_test(NAME aurocks.linked COMMAND aurocks_linked_test)
  if(TARGET aurocks)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME aurocks.integration COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py" "$<TARGET_FILE:aurocks>"
      "${CMAKE_C_COMPILER}")
    add_test(NAME aurocks.faults COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/faults.py" "$<TARGET_FILE:aurocks>")
    set(linked_case "${CMAKE_CURRENT_BINARY_DIR}/linked_generated.c")
    add_custom_command(OUTPUT "${linked_case}"
      COMMAND aurocks --backend libglr --entrypoint parse_linked
              -o "${linked_case}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/linked.g"
      DEPENDS aurocks tests/linked.g
      VERBATIM)
    add_executable(aurocks_linked_generated_test "${linked_case}")
    target_link_libraries(aurocks_linked_generated_test PRIVATE aurocks_runtime)
    set_target_properties(aurocks_linked_generated_test PROPERTIES
      C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    libstone_warnings(aurocks_linked_generated_test)
    add_test(NAME aurocks.linked_generated COMMAND aurocks_linked_generated_test)
  endif()
endif()
