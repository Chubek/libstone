execute_process(COMMAND ${GENERATOR_COMMAND} --no-strpool "${GRAMMAR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE parser ERROR_VARIABLE error)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "Aurocks failed (${status}): ${error}")
endif()
# Keep the runtime and yyparse private to this grammar's namespace, including
# shared-library builds linked with ExoML or other Aurocks users.
if(NOT DEFINED PREFIX)
  set(PREFIX novo_dl)
endif()
string(REPLACE "aurocks_" "${PREFIX}_aurocks_" parser "${parser}")
string(REPLACE "void *yyparse(" "void *${PREFIX}_yyparse(" parser "${parser}")
string(REPLACE "const char *yyparse_error(" "const char *${PREFIX}_yyparse_error(" parser "${parser}")
string(REPLACE "char *yyparse_error(" "char *${PREFIX}_yyparse_error(" parser "${parser}")
file(WRITE "${OUTPUT}" "${parser}")
