execute_process(COMMAND "${PERL}" "${GENERATOR}" --no-strpool "${GRAMMAR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE parser ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Aurocks: ${error}")
endif()
# Keep parser helpers private to the TraceML symbol namespace at link time.
string(REPLACE "aurocks_" "traceml_aurocks_" parser "${parser}")
string(REPLACE "void *yyparse(" "void *traceml_yyparse(" parser "${parser}")
file(WRITE "${OUTPUT}" "${parser}")
