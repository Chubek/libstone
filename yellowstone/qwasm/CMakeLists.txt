cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(qwasm LANGUAGES CXX)
  set(_qwasm_standalone ON)
  option(YELLOWSTONE_BUILD_TOOLS "Build the qwasm command-line tool" ON)
  option(YELLOWSTONE_BUILD_TESTS "Build qwasm tests" ON)
  if(YELLOWSTONE_BUILD_TESTS)
    enable_testing()
  endif()
endif()
include(GNUInstallDirs)

function(qwasm_warnings target)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
  if(QOBJFILE_WERROR)
    target_compile_options(${target} PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
  endif()
endfunction()

add_library(qwasm STATIC src/qwasm.cpp)
add_library(qwasm::qwasm ALIAS qwasm)
set_target_properties(qwasm PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(qwasm PUBLIC cxx_std_20)
target_include_directories(qwasm PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
qwasm_warnings(qwasm)

install(TARGETS qwasm EXPORT qwasmTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/qwasm/qwasm.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qwasm)
if(_qwasm_standalone)
  install(EXPORT qwasmTargets FILE qwasmConfig.cmake NAMESPACE qwasm::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qwasm)
endif()

if(YELLOWSTONE_BUILD_TOOLS)
  add_executable(qwasm_cli src/main.cpp)
  target_link_libraries(qwasm_cli PRIVATE qwasm::qwasm)
  set_target_properties(qwasm_cli PROPERTIES OUTPUT_NAME qwasm)
  qwasm_warnings(qwasm_cli)
  install(TARGETS qwasm_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(YELLOWSTONE_BUILD_TESTS)
  add_executable(qwasm_tests tests/inspect.cpp)
  target_link_libraries(qwasm_tests PRIVATE qwasm::qwasm)
  qwasm_warnings(qwasm_tests)
  add_test(NAME qwasm_inspect COMMAND qwasm_tests)
  set_tests_properties(qwasm_inspect PROPERTIES TIMEOUT 30)
  if(YELLOWSTONE_BUILD_TOOLS)
    add_test(NAME qwasm_cli
      COMMAND ${CMAKE_COMMAND}
        "-DQWASM=$<TARGET_FILE:qwasm_cli>"
        "-DQWASM_TESTS=$<TARGET_FILE:qwasm_tests>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake")
    set_tests_properties(qwasm_cli PROPERTIES TIMEOUT 30)
  endif()
endif()
