# C test suite: ports of the original nine tests plus DSL, plugin and
# fault-injection coverage. Every test is a standalone executable.
set(MemTKX_C_TESTS
  test_units
  test_allocator
  test_barrier
  test_immix
  test_large_object
  test_scheduler_edges
  test_dispatcher
  test_scheduler
  test_plans
  test_plugins
  test_dsl_values
  test_dsl_ast
  test_dsl_parser
  test_dsl_peg
  test_dsl_task
  test_oom
)

find_package(Threads REQUIRED)

foreach(test_name IN LISTS MemTKX_C_TESTS)
  add_executable(${test_name} c/${test_name}.c)
  target_link_libraries(${test_name} PRIVATE MemTKX::MemTKX Threads::Threads)
  set_target_properties(${test_name} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
                                                C_EXTENSIONS OFF)
  if(NOT WIN32)
    target_compile_definitions(${test_name} PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  if(MemTKX_ENABLE_WARNINGS)
    target_compile_options(${test_name} PRIVATE ${MemTKX_WARNING_FLAGS})
  endif()
  add_test(NAME c.${test_name} COMMAND ${test_name})
  set_tests_properties(c.${test_name} PROPERTIES TIMEOUT 60)
endforeach()

if(MemTKX_BUILD_CXX_WRAPPER)
  # The original C++ test suite, unchanged in spirit, now compiled against
  # the C++ wrapper over the C library. Tests using C++20 string-literal
  # template arguments are built as C++20; the rest as C++17.
  set(MemTKX_CPP17_TESTS
    test_units
    test_allocator
    test_barrier
    test_immix
    test_large_object
    test_scheduler_edges
    test_scheduler
    test_plans
    test_dispatcher
    test_interop
  )
  set(MemTKX_CPP20_TESTS
    test_dsl
  )
  foreach(test_name IN LISTS MemTKX_CPP17_TESTS MemTKX_CPP20_TESTS)
    add_executable(cpp_${test_name} cpp/${test_name}.cpp)
    target_link_libraries(cpp_${test_name} PRIVATE MemTKX::MemTKXCpp Threads::Threads)
    if(test_name IN_LIST MemTKX_CPP20_TESTS)
      target_compile_features(cpp_${test_name} PRIVATE cxx_std_20)
    endif()
    if(MemTKX_ENABLE_WARNINGS)
      target_compile_options(cpp_${test_name} PRIVATE ${MemTKX_WARNING_FLAGS})
    endif()
    add_test(NAME cpp.${test_name} COMMAND cpp_${test_name})
    set_tests_properties(cpp.${test_name} PROPERTIES TIMEOUT 60)
  endforeach()
endif()
