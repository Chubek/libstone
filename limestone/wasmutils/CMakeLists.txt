add_library(wasmutils STATIC WASM-Binutils.cpp WASM-Core2.cpp WASM-Core2-Opcodes.cpp
  WASM-Runtime.cpp WASM-CC.cpp WAT.cpp WIT.cpp)
target_link_libraries(wasmutils PUBLIC limestone_foundation)
target_compile_features(wasmutils PUBLIC cxx_std_20)
libstone_warnings(wasmutils)

if(LIMESTONE_BUILD_TOOLS OR LIMESTONE_BUILD_TESTS)
  add_executable(wasm2c WASM2C.cpp)
  add_executable(wat2c WAT2C.cpp)
  add_executable(wit2c WIT2C.cpp)
  add_executable(wasm-disassemble WASMDisassembler.cpp)
  foreach(tool IN ITEMS wasm2c wat2c wit2c wasm-disassemble)
    target_link_libraries(${tool} PRIVATE wasmutils)
    libstone_warnings(${tool})
  endforeach()
endif()

if(LIMESTONE_BUILD_TESTS)
  add_executable(limestone-wasmutils-test tests/wasmutils.cpp)
  target_link_libraries(limestone-wasmutils-test PRIVATE wasmutils)
  libstone_warnings(limestone-wasmutils-test)
  add_test(NAME wasmutils COMMAND limestone-wasmutils-test "${CMAKE_CURRENT_SOURCE_DIR}/WASM.aml")
  foreach(suite IN ITEMS binutils core2 runtime_memory)
    add_executable(limestone-wasmutils-${suite}-test tests/${suite}.cpp)
    target_link_libraries(limestone-wasmutils-${suite}-test PRIVATE wasmutils)
    libstone_warnings(limestone-wasmutils-${suite}-test)
    add_test(NAME wasmutils-${suite} COMMAND limestone-wasmutils-${suite}-test)
  endforeach()
  add_test(NAME wasmutils-disassemble COMMAND "${CMAKE_COMMAND}"
    "-DDISASSEMBLER=$<TARGET_FILE:wasm-disassemble>" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/disassemble.cmake")

  if(NOT CMAKE_CROSSCOMPILING AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME wasmutils-c90 COMMAND "${CMAKE_COMMAND}"
      "-DWASM2C=$<TARGET_FILE:wasm2c>" "-DWAT2C=$<TARGET_FILE:wat2c>"
      "-DWIT2C=$<TARGET_FILE:wit2c>" "-DFIXTURES=$<TARGET_FILE:limestone-wasmutils-test>"
      "-DCC=${CMAKE_C_COMPILER}" "-DCFLAGS=${CMAKE_C_FLAGS}"
      "-DLINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS}" "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/c90.cmake")
    set_tests_properties(wasmutils-c90 PROPERTIES TIMEOUT 30)
  endif()
endif()
