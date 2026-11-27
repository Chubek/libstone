execute_process(COMMAND "${EXAMPLE}" INPUT_FILE "${INPUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT status STREQUAL "0" OR NOT output MATCHES "\\(one two\\)"
   OR NOT output MATCHES "Use lambda")
  message(FATAL_ERROR "Lisp example failed (${status}): ${output}\n${error}")
endif()
