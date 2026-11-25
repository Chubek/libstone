cmake_minimum_required(VERSION 3.20)
project(LLSynthAssemblerTests LANGUAGES C CXX)
enable_testing()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
find_package(Sandstone CONFIG REQUIRED)

add_subdirectory(../../stdlib/assembler assembler)
add_executable(llsynth-assembler-mkbank ../interop/mkbank.cpp)
target_link_libraries(llsynth-assembler-mkbank PRIVATE Sandstone::sandstone)

add_test(NAME llsynth-std-assembler
  COMMAND "${CMAKE_COMMAND}" -E env
    "LLSYNTH_ASSEMBLER_LIBRARY=$<TARGET_FILE:llsynth_assembler>"
    "LLSYNTH_ASSEMBLER_MKBANK=$<TARGET_FILE:llsynth-assembler-mkbank>"
    "CC=${CMAKE_C_COMPILER}"
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_assembler.py")
set_tests_properties(llsynth-std-assembler PROPERTIES TIMEOUT 120)
