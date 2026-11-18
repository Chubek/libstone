%{
#include "TraceSourceAST.h"

/* The C++ wrapper serializes calls into the generated parser. */
static traceml_ast_arena *trace_arena;
typedef struct TraceForm {
  struct TraceForm *previous;
  traceml_ast_expr_t value;
  size_t count;
} TraceForm;
static TraceForm *trace_append(TraceForm *, traceml_ast_expr_t);
static traceml_ast_expr_seq trace_items(const TraceForm *);
static traceml_ast_expr_t trace_atom(char *, size_t);
static size_t trace_offset(GLRParser);
%}
%%
%start traceml_source
%skip /([ \t\r\n]+|;[^\n]*)+/

traceml_source: forms { $$ = traceml_ast_make_document(trace_arena, trace_items($1)); };
forms: forms expression { $$ = trace_append($1, $2); }
     | expression { $$ = trace_append(NULL, $1); };
expression: '(' forms ')' { $$ = traceml_ast_List(trace_arena, trace_items($2), trace_offset(save)); }
          | /[A-Za-z0-9_+*\/=<>!?-]+/ { $$ = trace_atom($1, trace_offset(save)); };
%%
static size_t trace_offset(GLRParser p) {
  glr_skip(&p);
  return (size_t)(p.s - p.begin);
}
static traceml_ast_expr_t trace_atom(char *text, size_t offset) {
  traceml_ast_string_t spelling = traceml_ast_string_copy(trace_arena, text, strlen(text));
  aurocks_free(text);
  return traceml_ast_Atom(trace_arena, spelling, offset);
}
static TraceForm *trace_append(TraceForm *previous, traceml_ast_expr_t value) {
  TraceForm *form;
  if (!value) return NULL;
  form = traceml_ast_arena_alloc(trace_arena, sizeof(*form));
  if (form) {
    form->previous = previous;
    form->value = value;
    form->count = 1 + (previous ? previous->count : 0);
  }
  return form;
}
static traceml_ast_expr_seq trace_items(const TraceForm *last) {
  traceml_ast_expr_seq result = traceml_ast_expr_seq_new(trace_arena, last ? last->count : 0);
  size_t i = result.length;
  if (!trace_arena->error) for (; last; last = last->previous) result.items[--i] = last->value;
  return result;
}
traceml_ast_document_t traceml_parse_source(const char *input, traceml_ast_arena *arena, size_t *offset) {
  traceml_ast_document_t document;
  const char *error;
  trace_arena = arena;
  document = parse_traceml_source(input);
  error = parse_traceml_source_error();
  *offset = error ? (size_t)(error - input) : 0;
  trace_arena = NULL;
  return arena->error ? NULL : document;
}
