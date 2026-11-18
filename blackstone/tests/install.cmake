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
set(dependency_prefix "${root}/dependency")
run("${CMAKE_COMMAND}" -S "${FIRESTONE_SOURCE}" -B "${root}/firestone"
  -DFIRESTONE_BUILD_TESTS=OFF -DLIBSTONE_BUILD_TESTS=OFF "-DCMAKE_INSTALL_PREFIX=${dependency_prefix}")
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
  using namespace firestone::ir;
  blackstone::Runtime runtime;
  runtime.load_directory(KERNEL_DIR);
  SimpleSSAGraph graph;
  const auto entry = graph.cfg().create_block();
  const auto int8 = Type::integer(8);
  const auto x = graph.parameter(int8);
  const auto negation = graph.define(entry, Opcode::Sub, int8,
                                    {Operand::constant(0, int8), Operand::ref(x)});
  graph.cfg().return_value(entry, Operand::ref(negation));
  const auto result = runtime.run_pipeline({"ext_zero_minus_x"}, graph,
                                           blackstone::IntegerSemantics::Modular, 2);
  return runtime.kernels().size() == 191 && check_c_header() &&
         result.changed && result.converged && result.rounds == 2 &&
         graph.cfg().block(entry).instructions.front().opcode == Opcode::Neg ? 0 : 1;
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
run("${CMAKE_COMMAND}" -S "${root}/consumer" -B "${root}/consumer-build"
  "-DCMAKE_PREFIX_PATH=${prefix}\;${dependency_prefix}")
run("${CMAKE_COMMAND}" --build "${root}/consumer-build" --parallel 2)
run("${root}/consumer-build/consumer")
