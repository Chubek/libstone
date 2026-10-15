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
set(D_IS_GDC FALSE)
if(D_NAME MATCHES "(^|-)gdc(-[0-9]+([.][0-9]+)*)?$")
  set(D_IS_GDC TRUE)
endif()
set(D_OUT "-of=${CMAKE_CURRENT_BINARY_DIR}/novoparse")
if(D_IS_GDC)
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

# Independent D reference library. Do not glob the Cache.d/LMDBX.d directories.
file(GLOB GATEML_D_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false
  "${CMAKE_CURRENT_SOURCE_DIR}/*.d" "${CMAKE_CURRENT_SOURCE_DIR}/serde/*.d")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  file(GLOB GATEML_D_IPC CONFIGURE_DEPENDS LIST_DIRECTORIES false "${CMAKE_CURRENT_SOURCE_DIR}/ipc/*.d")
  list(APPEND GATEML_D_SOURCES ${GATEML_D_IPC})
endif()
set(D_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/libgateml-d.a")
set(D_IMPORT "-I${CMAKE_CURRENT_SOURCE_DIR}/..")
if(D_IS_GDC)
  set(D_OBJECTS)
  foreach(source IN LISTS GATEML_D_SOURCES)
    file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${source}")
    string(REPLACE "/" "_" relative "${relative}")
    set(object "${CMAKE_CURRENT_BINARY_DIR}/d/${relative}.o")
    add_custom_command(OUTPUT "${object}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/d"
      COMMAND "${GATEML_D_COMPILER}" ${D_IMPORT} -g -c -o "${object}" "${source}"
      DEPENDS ${GATEML_D_SOURCES} VERBATIM)
    list(APPEND D_OBJECTS "${object}")
  endforeach()
  add_custom_command(OUTPUT "${D_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${D_LIBRARY}"
    COMMAND "${CMAKE_AR}" rcs "${D_LIBRARY}" ${D_OBJECTS}
    DEPENDS ${D_OBJECTS} VERBATIM)
else()
  add_custom_command(OUTPUT "${D_LIBRARY}"
    COMMAND "${GATEML_D_COMPILER}" ${D_IMPORT} -g -lib "-of=${D_LIBRARY}" ${GATEML_D_SOURCES}
    DEPENDS ${GATEML_D_SOURCES} VERBATIM)
endif()
add_custom_target(gateml-d ALL DEPENDS "${D_LIBRARY}")

if(BUILD_TESTING)
  find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
  add_executable(gateml-tests tests/frontend.cpp)
  target_link_libraries(gateml-tests PRIVATE gateml)
  target_compile_definitions(gateml-tests PRIVATE GATEBINDLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../gatebindle")
  add_test(NAME gateml.frontend COMMAND gateml-tests)
  add_test(NAME gateml.bundle COMMAND gateml-cli check "${CMAKE_CURRENT_SOURCE_DIR}/../gatebindle/C")
  foreach(bundle C C++ Delphi Java Python Ruby Rust Typescript Scheme)
    add_test(NAME gateml.bundle.${bundle} COMMAND gateml-cli check "${CMAKE_CURRENT_SOURCE_DIR}/../gatebindle/${bundle}")
  endforeach()
  add_test(NAME gateml.help COMMAND gateml-cli --help)
  add_test(NAME gateml.cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.py"
    "$<TARGET_FILE:gateml-cli>" "${CMAKE_CURRENT_BINARY_DIR}/novoparse"
    "${CMAKE_CURRENT_SOURCE_DIR}/../gatebindle" "${CMAKE_CURRENT_BINARY_DIR}")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # A custom target must not have the same basename as its generated binary
    # (Ninja's phony aliases would otherwise collide with the file's rule).
    set(D_TEST "${CMAKE_CURRENT_BINARY_DIR}/gateml-reference-tests")
    if(D_IS_GDC)
      set(D_TEST_OUT -o "${D_TEST}")
      set(D_TEST_LINK -lrt)
    else()
      set(D_TEST_OUT "-of=${D_TEST}")
      set(D_TEST_LINK -L-lrt)
    endif()
    add_custom_command(OUTPUT "${D_TEST}"
      COMMAND "${GATEML_D_COMPILER}" ${D_IMPORT} -g ${D_TEST_OUT}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference.d" "${D_LIBRARY}" ${D_TEST_LINK}
      DEPENDS tests/reference.d "${D_LIBRARY}" VERBATIM)
    add_custom_target(gateml-d-tests ALL DEPENDS "${D_TEST}")
    add_dependencies(gateml-d-tests gateml-d)
    add_test(NAME gateml.d-reference COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/interop.py" "${D_TEST}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../gatebindle" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(gateml.d-reference PROPERTIES TIMEOUT 60)
  endif()
endif()
