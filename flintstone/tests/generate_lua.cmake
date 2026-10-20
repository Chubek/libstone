execute_process(COMMAND "${XBIND}" --emit-lua Flintstone "${SOURCE}"
  OUTPUT_FILE "${OUTPUT}" ERROR_VARIABLE error RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Lua binding generation failed: ${error}")
endif()
