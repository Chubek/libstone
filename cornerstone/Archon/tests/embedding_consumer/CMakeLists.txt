cmake_minimum_required(VERSION 3.20)
project(ArchonInstalledConsumer LANGUAGES CXX)

find_package(Archon CONFIG REQUIRED)
find_package(Threads REQUIRED)
enable_testing()

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE Archon::Archon Threads::Threads)
configure_file(state.arch state.arch COPYONLY)
add_test(NAME archon-installed-consumer
    COMMAND consumer "${Archon_PYTHON_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(host_owned host_owned.cpp)
target_link_libraries(host_owned PRIVATE Archon::Archon)
add_test(NAME archon-host-owned-cpython COMMAND host_owned "${Archon_PYTHON_DIR}")
