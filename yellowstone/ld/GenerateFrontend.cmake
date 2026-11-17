file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${PERL}" "${SCRIPTS}/asdl.pl" --no-builtins
  --header "${OUT}/ldscript_ast.h" "${SOURCE}/ldscript.asdl"
  RESULT_VARIABLE status ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "ldscript ASDL generation failed: ${diagnostic}")
endif()
execute_process(COMMAND "${PERL}" "${SCRIPTS}/aurocks.pl" --no-strpool
  "${SOURCE}/ldscript.g"
  OUTPUT_FILE "${OUT}/ldscript_expression.c"
  RESULT_VARIABLE status ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "ldscript Aurocks generation failed: ${diagnostic}")
endif()
