execute_process(COMMAND "${PERL}" "${GENERATOR}" --no-strpool "${GRAMMAR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE parser ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Aurocks: ${error}")
endif()
# Keep the runtime and yyparse private to this grammar's namespace, including
# shared-library builds linked with ExoML or other Aurocks users.
string(REPLACE "aurocks_" "novo_dl_aurocks_" parser "${parser}")
string(REPLACE "void *yyparse(" "void *novo_dl_yyparse(" parser "${parser}")
file(WRITE "${OUTPUT}" "${parser}")
