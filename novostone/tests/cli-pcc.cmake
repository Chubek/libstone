if(NOT DEFINED CLI OR NOT DEFINED INPUT OR NOT DEFINED EXPECT_PROVED)
  message(FATAL_ERROR "CLI, INPUT, and EXPECT_PROVED are required")
endif()

execute_process(
  COMMAND "${CLI}" pcc "${INPUT}"
    --frontend smtlib2
    --transforms constant_folding,local_cse
    --require-proof
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)

foreach(required IN ITEMS
    "artifact-core [0-9]+"
    "constraints"
    "analysis"
    "transformations 1"
    "smt-code 1")
  if(NOT output MATCHES "${required}")
    message(FATAL_ERROR
      "PCC package is missing '${required}'\nstdout:\n${output}\nstderr:\n${error}")
  endif()
endforeach()

if(EXPECT_PROVED)
  if(NOT result EQUAL 0 OR NOT output MATCHES "unsat-proved")
    message(FATAL_ERROR
      "required Satie proof did not succeed (exit ${result})\n"
      "stdout:\n${output}\nstderr:\n${error}")
  endif()
else()
  if(NOT result EQUAL 3 OR NOT output MATCHES "unknown")
    message(FATAL_ERROR
      "solver-free required proof did not fail closed (exit ${result})\n"
      "stdout:\n${output}\nstderr:\n${error}")
  endif()
endif()
