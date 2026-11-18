execute_process(COMMAND "${PERL}" "${GENERATOR}" --no-strpool "${GRAMMAR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE parser ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Aurocks: ${error}")
endif()
# The generated helper names and yyparse must coexist with other Aurocks parsers.
string(REPLACE "aurocks_" "vmweave_aurocks_" parser "${parser}")
string(REPLACE "void *yyparse(" "void *vmweave_yyparse(" parser "${parser}")
file(WRITE "${OUTPUT}" "${parser}")
