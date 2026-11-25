# The generator embeds runtime.c in an IPC user's generated translation unit.
# This interface provides its host link requirements without building a DSO.
add_library(llsynth_ipc INTERFACE)
add_library(LLSynth::ipc ALIAS llsynth_ipc)
if(UNIX)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(llsynth_ipc INTERFACE Threads::Threads)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(llsynth_ipc INTERFACE rt)
endif()
