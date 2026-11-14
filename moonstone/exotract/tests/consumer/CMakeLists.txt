cmake_minimum_required(VERSION 3.20)
project(ExotractInstalledConsumer LANGUAGES CXX)
find_package(MoonstoneExotract CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE Moonstone::Exotract)
target_compile_definitions(consumer PRIVATE
  TEST_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/fixture.h"
  TEST_COMPILER="${TEST_COMPILER}"
  TEST_RESOURCES="${TEST_RESOURCES}")
if(TEST_EXTRACT)
  target_compile_definitions(consumer PRIVATE TEST_EXTRACT)
endif()
if(NOT MSVC)
  target_compile_options(consumer PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
endif()
enable_testing()
add_test(NAME installed_exotract COMMAND consumer)
