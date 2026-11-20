cmake_minimum_required(VERSION 3.20)
project(CephyrInstalledConsumer LANGUAGES C CXX)

enable_testing()
find_package(Cephyr 0.1 CONFIG REQUIRED)

add_executable(cephyr_c_api client.c)
set_target_properties(cephyr_c_api PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
target_link_libraries(cephyr_c_api PRIVATE Cephyr::Cephyr)
add_test(NAME cephyr_c_api COMMAND cephyr_c_api)

add_executable(cephyr_cpp_header client.cpp)
# The package must propagate its public header's language level, even when
# the application would otherwise default to C++14.
get_target_property(cephyr_features Cephyr::Cephyr INTERFACE_COMPILE_FEATURES)
if(NOT "cxx_std_17" IN_LIST cephyr_features)
  message(FATAL_ERROR "Cephyr::Cephyr does not publish its C++17 requirement")
endif()
set_target_properties(cephyr_cpp_header PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS OFF)
target_link_libraries(cephyr_cpp_header PRIVATE Cephyr::Cephyr)
add_test(NAME cephyr_cpp_header COMMAND cephyr_cpp_header)

if(CEPHYR_TEST_SDK_EXAMPLE)
  add_executable(cephyr_sdk_example "${CEPHYR_TEST_SDK_EXAMPLE}")
  set_target_properties(cephyr_sdk_example PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
  target_link_libraries(cephyr_sdk_example PRIVATE Cephyr::Cephyr)
  add_test(NAME cephyr_sdk_example COMMAND cephyr_sdk_example)
endif()
