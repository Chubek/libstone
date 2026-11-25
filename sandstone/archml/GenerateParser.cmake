execute_process(COMMAND ${GENERATOR_COMMAND} --no-strpool "${GRAMMAR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE parser ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Aurocks: ${error}")
endif()
# Aurocks exports a generic runtime and yyparse. Namespace those symbols so
# ArchML can coexist with other generated parsers in the same process.
string(REPLACE "aurocks_" "archml_aurocks_" parser "${parser}")
string(REPLACE "void *yyparse(" "void *archml_yyparse(" parser "${parser}")
string(REPLACE "char *yyparse_error(" "char *archml_yyparse_error(" parser "${parser}")
file(WRITE "${OUTPUT}" "${parser}")
