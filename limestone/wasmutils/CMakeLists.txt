add_library(wasmutils STATIC WASM-Binutils.cpp WASM-Core.cpp WASM-Core-Opcodes.cpp
  WASM-Module.cpp WASM-Validate.cpp WASM-Link.cpp WASM-Object-Link.cpp
  WASM-Instance.cpp WASM-Operations.cpp WASM-SIMD.cpp WASM-Runtime.cpp WASM-CC.cpp
  WAT.cpp WAT-Module.cpp WAT-Print.cpp WIT.cpp WIT-C.cpp WIT-Canonical.cpp)
target_link_libraries(wasmutils PUBLIC limestone_foundation)
target_compile_features(wasmutils PUBLIC cxx_std_20)
libstone_warnings(wasmutils)

if(LIMESTONE_BUILD_TOOLS OR LIMESTONE_BUILD_TESTS)
  add_executable(wasm2c WASM2C.cpp)
  add_executable(wat2c WAT2C.cpp)
  add_executable(wit2c WIT2C.cpp)
  add_executable(wasm-disassemble WASMDisassembler.cpp)
  add_executable(wasm-assemble WASMAssemble.cpp)
  add_executable(wasm-link WASMLink.cpp)
  add_executable(wasm-run WASMRun.cpp)
  add_executable(wasm-validate WASMValidate.cpp)
  set(wasmutils_tools wasm2c wat2c wit2c wasm-disassemble wasm-assemble wasm-link wasm-run wasm-validate)
  foreach(tool IN LISTS wasmutils_tools)
    target_link_libraries(${tool} PRIVATE wasmutils)
    libstone_warnings(${tool})
  endforeach()
endif()

if(LIMESTONE_BUILD_TESTS)
  add_executable(limestone-wasmutils-test tests/wasmutils.cpp)
  target_link_libraries(limestone-wasmutils-test PRIVATE wasmutils)
  libstone_warnings(limestone-wasmutils-test)
  add_test(NAME wasmutils COMMAND limestone-wasmutils-test "${CMAKE_CURRENT_SOURCE_DIR}/WASM.aml")
  set(wasmutils_test_targets limestone-wasmutils-test)
  foreach(suite IN ITEMS binutils core2 runtime_memory module validate link object_link
                         instance operations simd wat_module wit wit_c wit_canonical)
    set(target limestone-wasmutils-${suite}-test)
    add_executable(${target} tests/${suite}.cpp)
    target_link_libraries(${target} PRIVATE wasmutils)
    libstone_warnings(${target})
    add_test(NAME wasmutils-${suite} COMMAND ${target})
    list(APPEND wasmutils_test_targets ${target})
  endforeach()
  add_custom_target(wasmutils-check
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -C "$<CONFIG>" -R "^wasmutils"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS ${wasmutils_tools} ${wasmutils_test_targets}
    USES_TERMINAL)
  add_test(NAME wasmutils-disassemble COMMAND "${CMAKE_COMMAND}"
    "-DDISASSEMBLER=$<TARGET_FILE:wasm-disassemble>" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/disassemble.cmake")
  add_test(NAME wasmutils-module-cli COMMAND "${CMAKE_COMMAND}"
    "-DASSEMBLE=$<TARGET_FILE:wasm-assemble>" "-DDISASSEMBLE=$<TARGET_FILE:wasm-disassemble>"
    "-DLINK=$<TARGET_FILE:wasm-link>" "-DRUN=$<TARGET_FILE:wasm-run>"
    "-DVALIDATE=$<TARGET_FILE:wasm-validate>" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_cli.cmake")
  add_test(NAME wasmutils-canonical-cli COMMAND "${CMAKE_COMMAND}"
    "-DWIT2C=$<TARGET_FILE:wit2c>" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/canonical_cli.cmake")

  find_program(WASMUTILS_CLANG NAMES clang)
  if(WASMUTILS_CLANG AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME wasmutils-object-interop COMMAND "${CMAKE_COMMAND}"
      "-DCLANG=${WASMUTILS_CLANG}" "-DOBJECT_TEST=$<TARGET_FILE:limestone-wasmutils-object_link-test>"
      "-DLINK=$<TARGET_FILE:wasm-link>" "-DVALIDATE=$<TARGET_FILE:wasm-validate>"
      "-DRUN=$<TARGET_FILE:wasm-run>" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/object_interop.cmake")
    set_tests_properties(wasmutils-object-interop PROPERTIES
      SKIP_REGULAR_EXPRESSION "Skipping wasm32 object interoperability")
  endif()

  if(NOT CMAKE_CROSSCOMPILING AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME wasmutils-c90 COMMAND "${CMAKE_COMMAND}"
      "-DWASM2C=$<TARGET_FILE:wasm2c>" "-DWAT2C=$<TARGET_FILE:wat2c>"
      "-DWIT2C=$<TARGET_FILE:wit2c>" "-DFIXTURES=$<TARGET_FILE:limestone-wasmutils-test>"
      "-DCC=${CMAKE_C_COMPILER}" "-DCFLAGS=${CMAKE_C_FLAGS}"
      "-DLINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS}" "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/c90.cmake")
    set_tests_properties(wasmutils-c90 PROPERTIES TIMEOUT 30)
    add_test(NAME wasmutils-module-c90 COMMAND "${CMAKE_COMMAND}"
      "-DWASM2C=$<TARGET_FILE:wasm2c>" "-DWAT2C=$<TARGET_FILE:wat2c>"
      "-DASSEMBLE=$<TARGET_FILE:wasm-assemble>" "-DCC=${CMAKE_C_COMPILER}"
      "-DCFLAGS=${CMAKE_C_FLAGS}" "-DLINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
      "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}" "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_c90.cmake")
    add_test(NAME wasmutils-wit-c90 COMMAND "${CMAKE_COMMAND}"
      "-DWIT2C=$<TARGET_FILE:wit2c>" "-DWIT_C_TEST=$<TARGET_FILE:limestone-wasmutils-wit_c-test>"
      "-DCC=${CMAKE_C_COMPILER}" "-DCFLAGS=${CMAKE_C_FLAGS}" "-DLINK_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
      "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}" "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wit_c.cmake")
    set_tests_properties(wasmutils-module-c90 wasmutils-wit-c90 PROPERTIES TIMEOUT 30)
  endif()
endif()
