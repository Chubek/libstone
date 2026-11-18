cmake_minimum_required(VERSION 3.20)
set(fixtures "${CMAKE_CURRENT_BINARY_DIR}/qwasm-cli-fixtures")
file(MAKE_DIRECTORY "${fixtures}")
execute_process(COMMAND "${QWASM_TESTS}" --write-cli-fixtures "${fixtures}"
  COMMAND_ERROR_IS_FATAL ANY)

function(assert_contains text needle)
  string(FIND "${text}" "${needle}" index)
  if(index EQUAL -1)
    message(FATAL_ERROR "Expected '${needle}' in: ${text}")
  endif()
endfunction()

execute_process(COMMAND "${QWASM}" --help OUTPUT_VARIABLE output COMMAND_ERROR_IS_FATAL ANY)
assert_contains("${output}" "not full validation")

execute_process(COMMAND "${QWASM}" -- "${fixtures}/good input.wasm"
  OUTPUT_VARIABLE output ERROR_VARIABLE error COMMAND_ERROR_IS_FATAL ANY)
if(NOT error STREQUAL "")
  message(FATAL_ERROR "Unexpected stderr: ${error}")
endif()
assert_contains("${output}" "WebAssembly core binary version 1")
assert_contains("${output}" [=["x\x00\x1b\x0a\\\"" data @17 size=1]=])
assert_contains("${output}" [=["m" "f" function type=0]=])
assert_contains("${output}" [=["f" function index=0]=])

function(assert_failure expected_exit diagnostic)
  execute_process(COMMAND "${QWASM}" ${ARGN}
    RESULT_VARIABLE exit OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT exit STREQUAL expected_exit OR NOT output STREQUAL "")
    message(FATAL_ERROR "Expected exit ${expected_exit} and no stdout, got ${exit}: ${output}")
  endif()
  assert_contains("${error}" "${diagnostic}")
endfunction()

assert_failure(1 "malformed at byte 14: truncated export index" "${fixtures}/bad.wasm")
assert_failure(1 "unsupported at byte 4:" "${fixtures}/unsupported.wasm")
assert_failure(1 "could not open input" "${fixtures}/not-found.wasm")
assert_failure(2 "Usage: qwasm" --unknown)
