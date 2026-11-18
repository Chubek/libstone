set(file "${BINARY}/core2.ops")
# 04 40 | 41 01 | 05 | 41 02 | 0b | 0b
string(ASCII 4 64 65 1 5 65 2 11 11 bytes)
file(WRITE "${file}" "${bytes}")
execute_process(COMMAND "${DISASSEMBLER}" --expression -
  INPUT_FILE "${file}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "disassembler failed: ${errors}")
endif()
set(expected "0: if\n2: i32.const bits=0x1\n4: else\n5: i32.const bits=0x2\n7: end\n8: end\n")
if(NOT output STREQUAL expected)
  message(FATAL_ERROR "unexpected instruction listing: ${output}")
endif()
string(ASCII 253 154 2 bytes) # 0xfd 0x9a 0x02: outside Core 2.0
file(WRITE "${file}" "${bytes}")
execute_process(COMMAND "${DISASSEMBLER}" "${file}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT status STREQUAL "1" OR NOT output STREQUAL "" OR NOT errors MATCHES "byte offset 0")
  message(FATAL_ERROR "out-of-profile opcode was not rejected: ${output}${errors}")
endif()
