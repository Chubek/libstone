execute_process(COMMAND "${EXOML}" --emit Source "${SCRIPT}" OUTPUT_FILE "${OUTPUT}"
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "ExoML binding generation failed: ${result}")
endif()
