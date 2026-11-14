execute_process(COMMAND "${PERL}" "${SCRIPTS}/asdl.pl" --no-builtins --header "${OUTPUT}/exoml_ast.h"
  --visitor "${OUTPUT}/exoml_visitor.h" "${SOURCE}/ExoML.asdl" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "ExoML ASDL generation failed: ${result}")
endif()
execute_process(COMMAND "${PERL}" "${SCRIPTS}/aurocks.pl" --no-strpool "${SOURCE}/ExoML.g"
  OUTPUT_FILE "${OUTPUT}/exoml_parser.c" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "ExoML Aurocks generation failed: ${result}")
endif()
# Aurocks exports shared helper names by default. Namespace this generated TU
# so other generated LibStone parsers can be linked into the same executable.
file(READ "${OUTPUT}/exoml_parser.c" parser)
string(REPLACE "aurocks_" "exoml_aurocks_" parser "${parser}")
string(REPLACE "yyparse" "exoml_yyparse" parser "${parser}")
file(WRITE "${OUTPUT}/exoml_parser.c" "${parser}")
