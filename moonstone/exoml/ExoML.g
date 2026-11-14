%{
#include "exoml_ast.h"
#include "ParserBridge.h"
/* Aurocks has process-global parser state. Parser.cpp serializes this TU. */
static exoml_arena *arena;
static exoml_string_t captured(char *text) {
  exoml_string_t result = exoml_string_copy(arena, text, strlen(text));
  aurocks_free(text);
  return result;
}
static exoml_argument_t argument(char *text, exoml_argument_t previous) {
  return exoml_Argument(arena, captured(text),
      previous ? exoml_argument_some(previous) : exoml_argument_none());
}
%}
%%
%start exoml_program
%skip /([ \t\r\n]+|--[^\n]*)+/
exoml_program: statements { $$ = exoml_make_module(arena, $1 ? exoml_statement_some($1) : exoml_statement_none()); };
statements: %empty { $$ = NULL; }
  | statements statement {
      exoml_statement_t node = $2;
      if (node) node->v.Statement.previous = $1 ? exoml_statement_some($1) : exoml_statement_none();
      $$ = node;
    };
statement: /[A-Za-z_][A-Za-z0-9_]*/ /~(setconst|setvar|get|hatch)!/ /[A-Za-z_][A-Za-z0-9_]*(:[A-Za-z_][A-Za-z0-9_]*)*/ '(' arguments ')' ending {
  glr_skip(&save);
  $$ = exoml_Statement(arena, captured($1), captured($2), captured($3),
       $5 ? exoml_argument_some($5) : exoml_argument_none(), exoml_statement_none(),
       (size_t)(save.s - save.begin), (size_t)((const char *)$7 - save.begin));
};
ending: ';' { glr_skip(&save); $$ = (void *)(save.s + 1); };
arguments: items { $$ = $1; } | %empty { $$ = NULL; };
items: literal { $$ = $1; }
  | items ',' literal {
      exoml_argument_t node = $3;
      if (node) node->v.Argument.previous = exoml_argument_some($1);
      $$ = node;
    };
literal: /"([^"\\\r\n]|\\["\\nrt0])*"/ { $$ = argument($1, NULL); }
  | /\[[ \t\r\n]*("([^"\\\r\n]|\\["\\nrt0])*"[ \t\r\n]*(,[ \t\r\n]*"([^"\\\r\n]|\\["\\nrt0])*"[ \t\r\n]*)*)?\]/ { $$ = argument($1, NULL); }
  | /\$([A-Za-z_][A-Za-z0-9_]*|@)/ { $$ = argument($1, NULL); }
  | /-?[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?u?/ { $$ = argument($1, NULL); }
  | /true|false|null/ { $$ = argument($1, NULL); };
%%
int exoml_parse_commands(const char *text, size_t max_commands, size_t max_arguments,
                         exoml_command_callback callback, void *context, size_t *error_offset) {
  exoml_arena storage;
  exoml_module_t module;
  exoml_statement_t node;
  size_t count = 0;
  int status = 0;
  GLRParser parser;
  void *root = NULL;
  exoml_arena_init(&storage);
  arena = &storage;
  /* Drive the generated production directly: its generic public entry point
     replaces the furthest failure with the start of a trailing bad command. */
  parser.begin = text; parser.s = text; parser.end = text + strlen(text); parser.error = NULL;
  glr_error_pos = NULL;
  glr_depth = 0;
  module = glr_exoml_program(&parser, &root) ? root : NULL;
  glr_skip(&parser);
  if (parser.s != parser.end) {
    if (!glr_error_pos || parser.s > glr_error_pos) glr_error_pos = parser.s;
    module = NULL;
  }
  if (!module || storage.error) {
    const char *at = parse_exoml_program_error();
    *error_offset = at ? (size_t)(at - text) : 0;
    status = storage.error ? 3 : 1;
  } else {
    for (node = module->last.value; node; node = node->v.Statement.previous.value) {
      exoml_argument_t arg;
      size_t argc = 0;
      if (++count > max_commands) { status = 2; break; }
      for (arg = node->v.Statement.arguments.value; arg; arg = arg->v.Argument.previous.value)
        if (++argc > max_arguments) { status = 2; break; }
      if (status) break;
    }
    if (!status) {
      /* Reverse traversal is restored to source order by the C++ bridge. */
      for (node = module->last.value; node; node = node->v.Statement.previous.value) {
        exoml_argument_t arg;
        callback(context, 0, node->v.Statement.binding.data, node->v.Statement.operation.data,
                 node->v.Statement.service.data, node->v.Statement.begin, node->v.Statement.end);
        for (arg = node->v.Statement.arguments.value; arg; arg = arg->v.Argument.previous.value)
          callback(context, 1, arg->v.Argument.text.data, NULL, NULL, 0, 0);
      }
    }
  }
  exoml_arena_destroy(&storage);
  arena = NULL;
  return status;
}
