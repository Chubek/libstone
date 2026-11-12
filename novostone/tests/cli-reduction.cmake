file(WRITE "${WORK}/reduction-before.nir" "novostone 1 1 1\nblock 1\ninstr 2 param 0 0 0\ninstr 3 return 0 1 2 0\n")
file(WRITE "${WORK}/reduction-after.nir" "novostone 1 1 1\nblock 1\ninstr 2 const 0 0 0\ninstr 3 return 0 1 2 0\n")
execute_process(COMMAND "${CLI}" validate "${WORK}/reduction-before.nir" "${WORK}/reduction-after.nir" 728
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT status EQUAL 1 OR NOT output MATCHES "counterexample reduction:" OR
   NOT output MATCHES "arguments 1" OR NOT output MATCHES "greedy reduction complete")
  message(FATAL_ERROR "CLI did not report a reduced mismatch (exit ${status}):\n${output}\n${errors}")
endif()
