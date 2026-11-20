cmake_minimum_required(VERSION 3.20)
project(CephyrInstalledConsumer LANGUAGES C CXX)

enable_testing()
find_package(Cephyr 0.1 CONFIG REQUIRED)

add_executable(cephyr_c_api client.c)
set_target_properties(cephyr_c_api PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
target_link_libraries(cephyr_c_api PRIVATE Cephyr::Cephyr)
add_test(NAME cephyr_c_api COMMAND cephyr_c_api)

add_executable(cephyr_cpp_header client.cpp)
target_compile_features(cephyr_cpp_header PRIVATE cxx_std_20)
target_link_libraries(cephyr_cpp_header PRIVATE Cephyr::Cephyr)
add_test(NAME cephyr_cpp_header COMMAND cephyr_cpp_header)
