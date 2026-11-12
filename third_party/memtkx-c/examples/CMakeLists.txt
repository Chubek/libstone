# Complete example programs (also embedded in README.md / GUIDE.md).
set(MemTKX_C_EXAMPLES
  first_managed_object
  object_graph
  quota_space_plugin
  observed_plan_plugin
  peg_tokenizer
)
if(NOT WIN32)
  list(APPEND MemTKX_C_EXAMPLES safepoint_park) # uses POSIX threads directly
endif()

find_package(Threads REQUIRED)
foreach(example IN LISTS MemTKX_C_EXAMPLES)
  add_executable(example_${example} ${example}.c)
  target_link_libraries(example_${example} PRIVATE MemTKX::MemTKX Threads::Threads)
  set_target_properties(example_${example} PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  if(NOT WIN32)
    target_compile_definitions(example_${example} PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
endforeach()

if(MemTKX_BUILD_CXX_WRAPPER)
  foreach(example IN ITEMS cpp_wrapper cpp_plugin_from_c)
    add_executable(example_${example} ${example}.cpp)
    target_link_libraries(example_${example} PRIVATE MemTKX::MemTKXCpp)
  endforeach()
endif()
