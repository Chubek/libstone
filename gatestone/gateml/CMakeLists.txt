cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(GateML VERSION 0.1.0 LANGUAGES C CXX)
  include(CTest)
endif()
get_filename_component(LIBSTONE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(TP "${LIBSTONE_ROOT}/third_party")
include(cmake/GLR.cmake)
include(cmake/Chomsky.cmake)
add_library(gateml_novoparse STATIC "${TP}/novoparse/runtime/NovoParse.c")
target_include_directories(gateml_novoparse PUBLIC "${TP}/novoparse/runtime")
target_link_libraries(gateml_novoparse PUBLIC ${GATEML_GLR_TARGET})
find_program(GATEML_D_COMPILER NAMES ldc2 dmd gdc REQUIRED)
file(GLOB NP_SOURCES CONFIGURE_DEPENDS "${TP}/novoparse/source/*.d")
file(GLOB NP_PROFILES CONFIGURE_DEPENDS "${TP}/novoparse/profiles/*.sexp")
get_filename_component(D_NAME "${GATEML_D_COMPILER}" NAME)
set(D_OUT "-of=${CMAKE_CURRENT_BINARY_DIR}/novoparse")
if(D_NAME STREQUAL "gdc")
  set(D_OUT -o "${CMAKE_CURRENT_BINARY_DIR}/novoparse")
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/novoparse"
  COMMAND "${GATEML_D_COMPILER}" "-I${TP}/novoparse/source"
    "-J${TP}/novoparse/profiles" ${D_OUT} ${NP_SOURCES}
  DEPENDS ${NP_SOURCES} ${NP_PROFILES} VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GateMLParser.hpp"
  COMMAND "${CMAKE_CURRENT_BINARY_DIR}/novoparse" generate
    "${CMAKE_CURRENT_SOURCE_DIR}/GateML.g" --profile C++
    -o "${CMAKE_CURRENT_BINARY_DIR}/GateMLParser.hpp"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/novoparse" GateML.g VERBATIM)
add_library(gateml STATIC src/GateML.cpp "${CMAKE_CURRENT_BINARY_DIR}/GateMLParser.hpp")
target_compile_features(gateml PUBLIC cxx_std_17)
target_include_directories(gateml PUBLIC include "${LIBSTONE_ROOT}/jiterati/include"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(gateml PUBLIC gateml_novoparse PRIVATE ${GATEML_CHOMSKY_TARGET})
add_executable(gateml-cli src/main.cpp)
target_link_libraries(gateml-cli PRIVATE gateml)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(gateml-tests tests/frontend.cpp)
  target_link_libraries(gateml-tests PRIVATE gateml)
  target_compile_definitions(gateml-tests PRIVATE GATEBINDLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../gatebindle")
  add_test(NAME gateml.frontend COMMAND gateml-tests)
  add_test(NAME gateml.bundle COMMAND gateml-cli check "${CMAKE_CURRENT_SOURCE_DIR}/../gatebindle/C")
  foreach(bundle C C++ Delphi Java Python Ruby Rust)
    add_test(NAME gateml.bundle.${bundle} COMMAND gateml-cli check "${CMAKE_CURRENT_SOURCE_DIR}/../gatebindle/${bundle}")
  endforeach()
  add_test(NAME gateml.help COMMAND gateml-cli --help)
  add_test(NAME gateml.cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.py"
    "$<TARGET_FILE:gateml-cli>" "${CMAKE_CURRENT_BINARY_DIR}/novoparse"
    "${CMAKE_CURRENT_SOURCE_DIR}/../gatebindle" "${CMAKE_CURRENT_BINARY_DIR}")
endif()
