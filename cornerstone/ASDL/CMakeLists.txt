cmake_minimum_required(VERSION 3.20)
project(ASDL VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)

option(ASDL_BUILD_TOOLS "Build ASDL command-line tools" ON)
option(ASDL_BUILD_TESTS "Build ASDL tests" "${BUILD_TESTING}")
option(ASDL_BUILD_MQCTL "Build developer POSIX message-queue tool when available" ON)

if(ASDL_BUILD_TESTS)
  enable_testing()
  find_package(Perl QUIET)
  if(PERL_FOUND AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME asdl.legacy
      COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}"
        "${PERL_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/asdl.t")
    set_tests_properties(asdl.legacy PROPERTIES TIMEOUT 120)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND AND CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$"
       AND NOT CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
      add_test(NAME asdl.downstream-legacy
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/downstream.py"
          --root "${CMAKE_CURRENT_SOURCE_DIR}/../.." --work-dir "${CMAKE_CURRENT_BINARY_DIR}"
          --perl "${PERL_EXECUTABLE}" --cc "${CMAKE_C_COMPILER}")
      set_tests_properties(asdl.downstream-legacy PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()
  endif()
endif()

if(ASDL_BUILD_TOOLS AND ASDL_BUILD_MQCTL)
  include(cmake/MessageQueue.cmake)
endif()

include(cmake/Native.cmake)
