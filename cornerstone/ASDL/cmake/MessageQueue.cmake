include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

# Older POSIX implementations provide the queue functions in librt, newer
# glibc provides them directly. Test linking, not just header availability.
set(_asdl_mq_probe [[
  #include <mqueue.h>
  #include <time.h>
  int main() {
    timespec t{};
    char byte{};
    mqd_t q = mq_open("/asdl_probe", O_RDONLY);
    return clock_gettime(CLOCK_REALTIME, &t) + mq_timedreceive(q, &byte, 1, nullptr, &t)
        + mq_timedsend(q, &byte, 1, 0, &t) + mq_close(q) + mq_unlink("/asdl_probe");
  }
]])
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_DEFINITIONS -D_POSIX_C_SOURCE=200809L)
check_cxx_source_compiles("${_asdl_mq_probe}" ASDL_HAVE_POSIX_MQ)
if(NOT ASDL_HAVE_POSIX_MQ)
  set(CMAKE_REQUIRED_LIBRARIES rt)
  check_cxx_source_compiles("${_asdl_mq_probe}" ASDL_HAVE_POSIX_MQ_RT)
endif()
cmake_pop_check_state()
unset(_asdl_mq_probe)

if(ASDL_HAVE_POSIX_MQ OR ASDL_HAVE_POSIX_MQ_RT)
  add_executable(mqctl tools/mqctl.cpp)
  target_compile_features(mqctl PRIVATE cxx_std_20)
  target_compile_definitions(mqctl PRIVATE _POSIX_C_SOURCE=200809L)
  if(NOT ASDL_HAVE_POSIX_MQ)
    target_link_libraries(mqctl PRIVATE rt)
  endif()
  if(ASDL_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      add_test(NAME asdl.mqctl
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mqctl.py"
          "$<TARGET_FILE:mqctl>")
      set_tests_properties(asdl.mqctl PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    endif()
  endif()
else()
  message(STATUS "ASDL mqctl disabled: POSIX message queues are unavailable")
endif()
