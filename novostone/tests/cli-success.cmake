execute_process(COMMAND "${CLI}" ${ARGUMENTS}
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT "${status}" STREQUAL "0")
  message(FATAL_ERROR "CLI failed (${status}): ${output}\n${error}")
endif()
if(NOT output MATCHES "${EXPECT}")
  message(FATAL_ERROR "CLI output does not match '${EXPECT}': ${output}\n${error}")
endif()
