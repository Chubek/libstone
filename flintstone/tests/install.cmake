string(RANDOM LENGTH 8 ALPHABET abcdef0123456789 suffix)
set(prefix "${PREFIX_ROOT}/install-${suffix}")
set(relocated "${PREFIX_ROOT}/relocated-${suffix}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BINARY}" --prefix "${prefix}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Installation failed: ${output}\n${error}")
endif()
file(RENAME "${prefix}" "${relocated}")
set(consumer "${PREFIX_ROOT}/consumer-${suffix}")
file(MAKE_DIRECTORY "${consumer}")
file(WRITE "${consumer}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.20)\nproject(InstalledConsumer LANGUAGES C CXX)\nfind_package(Flintstone CONFIG REQUIRED)\nadd_executable(consumer main.cpp c.c)\ntarget_link_libraries(consumer PRIVATE Flintstone::native)\n")
file(WRITE "${consumer}/c.c" "#include <Flintstone.h>\nint c_header(void) {return flintstone_binding_target_count() == 9;}\n")
file(WRITE "${consumer}/main.cpp" [=[
#include <Flintstone.hpp>
#include <targets/Native.hpp>
#include <frontend/visitor.hpp>
#include <iostream>
extern "C" int c_header(void);
int main() {
  flintstone::Registry registry;
  auto module = flintstone::parse("module m level core {import \"math\";func @f()->i32{^e:%x:i32=func.call(6:i32,7:i32) attrs {callee=\"std.math.max_i32\";};%r:i32=arith.mul(%x,6:i32);func.return(%r);}}");
  flintstone::resolve_imports(module, registry);
  domsymas::Builder bank;
  if (!c_header() || std::get<std::int64_t>(flintstone::execute(module,registry,"f")) != 42) return 1;
  return 0;
}
]=])
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${consumer}" -B "${consumer}/build"
  "-DCMAKE_PREFIX_PATH=${relocated}" "-DCMAKE_C_COMPILER=${CC}" "-DCMAKE_CXX_COMPILER=${CXX}"
  "-DCMAKE_C_FLAGS=${C_FLAGS}" "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}" "-DCMAKE_EXE_LINKER_FLAGS=${LINK_FLAGS}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Relocated package configuration failed: ${output}\n${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${consumer}/build" --parallel 2
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Installed consumer build failed: ${output}\n${error}")
endif()
execute_process(COMMAND "${consumer}/build/consumer" RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Installed consumer execution failed: ${error}")
endif()
