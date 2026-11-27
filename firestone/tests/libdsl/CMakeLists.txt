# Copy only the public header to catch accidental dependencies on repository files.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../../libdsl/LibDSL.hpp
  ${CMAKE_CURRENT_BINARY_DIR}/standalone/LibDSL.hpp COPYONLY)

add_custom_target(firestone_dsl_tests)

function(firestone_dsl_test name)
  add_executable(firestone_dsl_${name}_test ${ARGN})
  add_dependencies(firestone_dsl_tests firestone_dsl_${name}_test)
  target_compile_features(firestone_dsl_${name}_test PRIVATE cxx_std_20)
  target_include_directories(firestone_dsl_${name}_test PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/standalone)
  set_target_properties(firestone_dsl_${name}_test PROPERTIES CXX_EXTENSIONS OFF)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(firestone_dsl_${name}_test PRIVATE
      -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  endif()
  add_test(NAME firestone.dsl.${name} COMMAND firestone_dsl_${name}_test)
  set_tests_properties(firestone.dsl.${name} PROPERTIES TIMEOUT 30)
endfunction()

firestone_dsl_test(odr standalone.cpp standalone_peer.cpp)
firestone_dsl_test(portable standalone.cpp standalone_peer.cpp)
target_compile_definitions(firestone_dsl_portable_test PRIVATE LIBDSL_LIVE_POSIX=0)

foreach(suite IN ITEMS support syntax terminfo escape cli repl analysis scripts specification attributes execution optimization pmr keymap hoare linking)
  firestone_dsl_test(${suite} ${suite}.cpp)
endforeach()
target_link_libraries(firestone_dsl_scripts_test PRIVATE Threads::Threads)
firestone_dsl_test(terminfo_portable terminfo.cpp)
target_compile_definitions(firestone_dsl_terminfo_portable_test PRIVATE LIBDSL_LIVE_POSIX=0)
firestone_dsl_test(scripts_portable scripts.cpp)
target_compile_definitions(firestone_dsl_scripts_portable_test PRIVATE LIBDSL_LIVE_POSIX=0)
target_link_libraries(firestone_dsl_scripts_portable_test PRIVATE Threads::Threads)

if(FIRESTONE_BUILD_DSL_EXAMPLES)
  add_dependencies(firestone_dsl_tests firestone_dsl_archive_cli firestone_dsl_live_lisp)
  add_test(NAME firestone.dsl.archive_example
    COMMAND firestone_dsl_archive_cli files run -xvf archive.tar member)
  set_tests_properties(firestone.dsl.archive_example PROPERTIES
    PASS_REGULAR_EXPRESSION "extract archive\\.tar member")
  add_test(NAME firestone.dsl.lisp_example
    COMMAND ${CMAKE_COMMAND}
      "-DEXAMPLE=$<TARGET_FILE:firestone_dsl_live_lisp>"
      "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/live_lisp.input"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/example.cmake)
endif()
