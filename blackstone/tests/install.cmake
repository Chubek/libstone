cmake_minimum_required(VERSION 3.20)
function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Command failed: ${ARGV}\n${output}\n${error}")
  endif()
endfunction()
set(root "${BLACKSTONE_BINARY}/install-check")
file(REMOVE_RECURSE "${root}")
set(prefix "${root}/prefix")
run("${CMAKE_COMMAND}" -S "${FIRESTONE_SOURCE}" -B "${root}/firestone"
  -DFIRESTONE_BUILD_TESTS=OFF -DLIBSTONE_BUILD_TESTS=OFF "-DCMAKE_INSTALL_PREFIX=${prefix}")
run("${CMAKE_COMMAND}" --install "${root}/firestone")
run("${CMAKE_COMMAND}" --install "${BLACKSTONE_BINARY}" --prefix "${prefix}")
file(MAKE_DIRECTORY "${root}/consumer")
file(WRITE "${root}/consumer/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(BlackstoneConsumer LANGUAGES C CXX)
find_package(Blackstone 0.2 CONFIG REQUIRED)
add_executable(consumer main.cpp c_api.c)
target_link_libraries(consumer PRIVATE Blackstone::blackstone Blackstone::lua)
set_target_properties(consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(consumer PRIVATE KERNEL_DIR="${Blackstone_KERNEL_DIR}")
]=])
file(WRITE "${root}/consumer/main.cpp" [=[
#include <blackstone.hpp>
extern "C" int check_c_header(void);
int main() {
  blackstone::Runtime runtime;
  runtime.load_directory(KERNEL_DIR);
  return runtime.kernels().size() == 71 && check_c_header() ? 0 : 1;
}
]=])
file(WRITE "${root}/consumer/c_api.c" [=[
#include <blackstone.h>
#include <lua.h>
int check_c_header(void) {
  blackstone_runtime *r = blackstone_create();
  if (!r) return 0;
  blackstone_destroy(r);
  return LUA_VERSION_NUM >= 503;
}
]=])
run("${CMAKE_COMMAND}" -S "${root}/consumer" -B "${root}/consumer-build" "-DCMAKE_PREFIX_PATH=${prefix}")
run("${CMAKE_COMMAND}" --build "${root}/consumer-build" --parallel 2)
run("${root}/consumer-build/consumer")
