file(MAKE_DIRECTORY "${OUT}")
# Preserve the script entry point used by standalone/Make builds.
if(NOT DEFINED ASDL)
  set(ASDL "${SCRIPTS}/asdl.pl")
endif()
if(NOT DEFINED AUROCKS)
  set(AUROCKS "${SCRIPTS}/aurocks.pl")
endif()
execute_process(COMMAND "${PERL}" "${ASDL}" --no-builtins
  --header "${OUT}/ldscript_ast.h" "${SOURCE}/ldscript.asdl"
  RESULT_VARIABLE status ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "ldscript ASDL generation failed: ${diagnostic}")
endif()
execute_process(COMMAND "${PERL}" "${AUROCKS}" --no-strpool
  "${SOURCE}/ldscript.g"
  OUTPUT_FILE "${OUT}/ldscript_expression.c"
  RESULT_VARIABLE status ERROR_VARIABLE diagnostic)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "ldscript Aurocks generation failed: ${diagnostic}")
endif()
