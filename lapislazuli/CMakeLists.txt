cmake_minimum_required(VERSION 3.20)
project(Lapislazuli VERSION 0.1.0 LANGUAGES C)

# Build checked-in C without Lua, binutils, downloads or regeneration.
file(GLOB collection_sources CONFIGURE_DEPENDS
  "ArchCollection/*/*.c" "BFDCollection/*/*.c" "VMCollection/*/*.c")
add_library(lapislazuli_collections ${collection_sources})
add_library(Lapislazuli::Collections ALIAS lapislazuli_collections)
target_compile_features(lapislazuli_collections PUBLIC c_std_99)
target_include_directories(lapislazuli_collections PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
set_target_properties(lapislazuli_collections PROPERTIES C_EXTENSIONS OFF)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  find_program(LAPISLAZULI_LUA NAMES lua5.4 lua REQUIRED)
  execute_process(COMMAND "${LAPISLAZULI_LUA}" -e "assert(_VERSION == 'Lua 5.4')"
    RESULT_VARIABLE lua_status)
  if(NOT lua_status EQUAL 0)
    message(FATAL_ERROR "Generation tests require Lua 5.4; set LAPISLAZULI_LUA")
  endif()
  add_test(NAME lapislazuli-native-collections
    COMMAND "${CMAKE_COMMAND}" -E env "LUA=${LAPISLAZULI_LUA}" "CC=${CMAKE_C_COMPILER}"
      "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_collections.py")
  add_test(NAME lapislazuli-vm-collection
    COMMAND "${CMAKE_COMMAND}" -E env "LUA=${LAPISLAZULI_LUA}" "CC=${CMAKE_C_COMPILER}"
      "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/VMCollection/test_collection.py")
endif()
