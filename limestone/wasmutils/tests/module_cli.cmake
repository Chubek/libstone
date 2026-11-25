set(dir "${BINARY}/module-cli")
file(MAKE_DIRECTORY "${dir}")

macro(run_ok)
  execute_process(${ARGN} RESULT_VARIABLE status ERROR_VARIABLE error)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "command failed (${status}): ${ARGN}\n${error}")
  endif()
endmacro()

file(WRITE "${dir}/math.wat" [=[
(module
  (func (export "twice") (param $x i32) (result i32)
    local.get $x i32.const 2 i32.mul))
]=])
file(WRITE "${dir}/program.wat" [=[
(module
  (import "math" "twice" (func $twice (param i32) (result i32)))
  (memory 1)
  (data (i32.const 30) "\15\00")
  (func (export "run") (param $yes i32) (result i32)
    local.get $yes
    if (result i32)
      i32.const 30
      i32.load16_u
      call $twice
    else
      i32.const 0
    end)
  (func (export "vector") (param v128) (result v128)
    local.get 0
    v128.const i64x2 1 2
    i64x2.add)
  (func (export "forever") (loop $l (br $l))))
]=])
foreach(input IN ITEMS math program)
  run_ok(COMMAND "${ASSEMBLE}" "${dir}/${input}.wat" OUTPUT_FILE "${dir}/${input}.wasm")
  run_ok(COMMAND "${VALIDATE}" "${dir}/${input}.wasm")
endforeach()
run_ok(COMMAND "${LINK}" "program=${dir}/program.wasm" "math=${dir}/math.wasm"
  OUTPUT_FILE "${dir}/linked.wasm")
run_ok(COMMAND "${VALIDATE}" "${dir}/linked.wasm")
run_ok(COMMAND "${RUN}" "${dir}/linked.wasm" run i32:1 OUTPUT_VARIABLE result)
if(NOT result STREQUAL "i32:0x0000002a\n")
  message(FATAL_ERROR "unexpected execution result: ${result}")
endif()
run_ok(COMMAND "${RUN}" "${dir}/linked.wasm" run i32:0 OUTPUT_VARIABLE result)
if(NOT result STREQUAL "i32:0x00000000\n")
  message(FATAL_ERROR "unexpected execution result: ${result}")
endif()
run_ok(COMMAND "${RUN}" "${dir}/linked.wasm" vector v128:0x1234567890abcdefffffffffffffffff
  OUTPUT_VARIABLE result)
if(NOT result STREQUAL "v128:0x1234567890abcdf10000000000000000\n")
  message(FATAL_ERROR "unexpected SIMD execution result: ${result}")
endif()
execute_process(COMMAND "${RUN}" --fuel 5 "${dir}/linked.wasm" forever
  OUTPUT_VARIABLE result ERROR_VARIABLE error RESULT_VARIABLE status)
if(status STREQUAL "0" OR NOT result STREQUAL "" OR NOT error MATCHES "fuel")
  message(FATAL_ERROR "fuel exhaustion failed (${status}): ${result} ${error}")
endif()

run_ok(COMMAND "${DISASSEMBLE}" --wat "${dir}/linked.wasm" OUTPUT_FILE "${dir}/printed.wat")
run_ok(COMMAND "${ASSEMBLE}" "${dir}/printed.wat" OUTPUT_FILE "${dir}/roundtrip.wasm")
run_ok(COMMAND "${CMAKE_COMMAND}" -E compare_files "${dir}/linked.wasm" "${dir}/roundtrip.wasm")

file(WRITE "${dir}/invalid.wat" "(module\n  (func (result i64) i32.const 1))\n")
execute_process(COMMAND "${ASSEMBLE}" "${dir}/invalid.wat"
  OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status)
if(status STREQUAL "0" OR NOT output STREQUAL "" OR NOT error MATCHES "invalid\\.wat:2:")
  message(FATAL_ERROR "invalid module was not diagnosed at line 2 (${status}): ${error}")
endif()
