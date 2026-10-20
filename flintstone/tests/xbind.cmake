execute_process(COMMAND "${XBIND}" --check-spec Flintstone "${SOURCE}" RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Flintstone XBind spec failed: ${error}")
endif()
execute_process(COMMAND "${XBIND}" --emit-check Flintstone "${SOURCE}"
  RESULT_VARIABLE status OUTPUT_FILE "${BINARY}/xbind-signatures.cpp" ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "XBind signature generation failed: ${error}")
endif()
execute_process(COMMAND "${CXX}" -std=c++20 -I "${SOURCE}" -c "${BINARY}/xbind-signatures.cpp"
  -o "${BINARY}/xbind-signatures.o" RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Generated XBind declarations disagree with the C ABI: ${error}")
endif()
