foreach(vendor IN ITEMS MEMTKX SATIE EQUINOX)
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${TEST_DIR}/${vendor}"
    -DMOONSTONE_USE_${vendor}=ON -DMOONSTONE_BUILD_TESTS=OFF -DMOONSTONE_BUILD_TOOLS=OFF
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(result EQUAL 0 OR NOT "${output}${errors}" MATCHES "executable adapter is not implemented")
    message(FATAL_ERROR "${vendor} must fail explicitly, got: ${output}${errors}")
  endif()
endforeach()
