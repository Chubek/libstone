cmake_minimum_required(VERSION 3.20)
project(MetaVect VERSION 0.1.0 LANGUAGES C CXX)

option(METAVECT_BUILD_C_API "Build the MetaVect library" ON)
option(METAVECT_BUILD_CLI "Build the metavectc command-line compiler" ON)
option(METAVECT_BUILD_TESTS "Build the MetaVect tests" ON)
option(METAVECT_BUILD_D_TEST "Test the D binding with LDC or DMD when available" ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(METAVECT_BUILD_C_API)
  add_library(metavect STATIC
    MetaVect.c
    support.c
    frontend/frontend.c
    frontend/semantic.c
    mvir/verify.c
    mvir/rewrite.c
    stdbe/emitter.c)
  target_include_directories(metavect PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_features(metavect PUBLIC c_std_11)
  if(UNIX)
    target_link_libraries(metavect PUBLIC m)
  endif()
  if(METAVECT_BUILD_CLI)
    add_executable(metavectc tools/metavectc.c)
    target_link_libraries(metavectc PRIVATE metavect)
  endif()
endif()

include(CTest)
if(BUILD_TESTING AND METAVECT_BUILD_TESTS)
  if(METAVECT_BUILD_C_API)
    foreach(test IN ITEMS c_api cpp_api frontend semantic backends numbers)
      set(extension c)
      if(test STREQUAL "cpp_api")
        set(extension cpp)
      endif()
      add_executable(metavect_test_${test} tests/test_${test}.${extension})
      target_link_libraries(metavect_test_${test} PRIVATE metavect)
      add_test(NAME metavect_${test} COMMAND metavect_test_${test})
    endforeach()
    if(METAVECT_BUILD_D_TEST AND UNIX AND NOT CMAKE_CROSSCOMPILING)
      find_program(METAVECT_D_COMPILER NAMES ldc2 dmd)
      if(METAVECT_D_COMPILER)
        set(d_test_dir "${CMAKE_CURRENT_BINARY_DIR}/d-test/$<CONFIG>")
        set(d_test "${d_test_dir}/metavect_test_d_api")
        add_custom_command(OUTPUT "${d_test}"
          COMMAND "${CMAKE_COMMAND}" -E make_directory "${d_test_dir}"
          COMMAND "${METAVECT_D_COMPILER}" "-I=${CMAKE_CURRENT_SOURCE_DIR}"
            "-of=${d_test}" "-od=${d_test_dir}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_d_api.d" "$<TARGET_FILE:metavect>" -L-lm
          DEPENDS metavect MetaVect.di tests/test_d_api.d
          VERBATIM)
        add_custom_target(metavect_test_d_api ALL DEPENDS "${d_test}")
        add_test(NAME metavect_d_api COMMAND "${d_test}")
      endif()
    endif()
    if(TARGET metavectc)
      find_program(METAVECT_GHOSTSCRIPT NAMES gs gswin64c)
      add_test(NAME metavect_cli
        COMMAND "${CMAKE_COMMAND}"
          "-DCLI=$<TARGET_FILE:metavectc>"
          "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/shapes.mvct"
          "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-test"
          "-DGHOSTSCRIPT=${METAVECT_GHOSTSCRIPT}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.cmake")
    endif()
  endif()
  add_test(NAME metavect_source_tree_has_readme
    COMMAND ${CMAKE_COMMAND} -E env "METAVECT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check-source.cmake")
endif()
