set(MemTKX_BENCHMARKS
  bench_alloc
  bench_barrier
)

foreach(bench_name IN LISTS MemTKX_BENCHMARKS)
  add_executable(${bench_name} ${bench_name}.c)
  target_link_libraries(${bench_name} PRIVATE MemTKX::MemTKX)
  set_target_properties(${bench_name} PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  if(NOT WIN32)
    target_compile_definitions(${bench_name} PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${bench_name} PRIVATE -O3)
  endif()
endforeach()
