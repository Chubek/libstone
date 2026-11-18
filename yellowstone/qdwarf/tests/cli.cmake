execute_process(COMMAND "${FIXTURES}" --write-fixture "${INPUT}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Could not write CLI fixture: ${status}")
endif()
execute_process(COMMAND "${CLI}" --units --lines --address 0x1001 "${INPUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT output MATCHES "DWARF32 v4 legacy-info"
    OR NOT output MATCHES "0x100a op-index=0 end_sequence"
    OR NOT output MATCHES "0x1001: directory=\"src\" path=\"a.c\":3:7"
    OR NOT error STREQUAL "")
  message(FATAL_ERROR "Unexpected CLI lookup: ${status}\n${output}\n${error}")
endif()
execute_process(COMMAND "${CLI}" --address 4097 "${INPUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT output MATCHES "0x1001: directory=\"src\" path=\"a.c\":3:7"
    OR output MATCHES "DWARF32" OR NOT error STREQUAL "")
  message(FATAL_ERROR "Unexpected address-only CLI lookup: ${status}\n${output}\n${error}")
endif()
execute_process(COMMAND "${CLI}" --address 0x100a "${INPUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 1 OR NOT output MATCHES "0x100a: no source row")
  message(FATAL_ERROR "Unexpected CLI lookup at end_sequence: ${status}\n${output}\n${error}")
endif()
execute_process(COMMAND "${CLI}" --address -1 "${INPUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 2 OR NOT error MATCHES "invalid address")
  message(FATAL_ERROR "CLI accepted an invalid address: ${status}\n${output}\n${error}")
endif()
file(REMOVE "${INPUT}")
