%{
#include "AbstractAST.h"

/* Calls are serialized by parse_abstract: Aurocks has static parser state. */
static vw_ast_arena *vw_arena;
typedef struct VWLink {
  struct VWLink *previous;
  vw_ast_operation_t operation;
  size_t count;
} VWLink;
static VWLink *vw_append(VWLink *, vw_ast_operation_t);
static vw_ast_string_t vw_text(char *);
static size_t vw_offset(GLRParser);
%}
%%
%start vw_abstract_program
%skip /([ \t\r\n]+|#[^\r\n]*)+/

vw_abstract_program: items { $$ = vw_append($1, NULL); };
items: items operation { $$ = vw_append($1, $2); }
     | %empty { $$ = NULL; };
operation: /[a-z][a-z0-9_]*/ ';'
    { $$ = vw_ast_Bare(vw_arena, vw_text($1), vw_offset(save)); }
  | /[a-z][a-z0-9_]*/ /-?[0-9]+/ ';'
    { $$ = vw_ast_Numbered(vw_arena, vw_text($1), vw_text($2), vw_offset(save)); }
  | /[a-z][a-z0-9_]*/ /[A-Za-z][A-Za-z0-9_]*/ ';'
    { $$ = vw_ast_Named(vw_arena, vw_text($1), vw_text($2), vw_offset(save)); };
%%
static size_t vw_offset(GLRParser p) {
  glr_skip(&p);
  return (size_t)(p.s - p.begin);
}
static vw_ast_string_t vw_text(char *text) {
  vw_ast_string_t result = vw_ast_string_copy(vw_arena, text, strlen(text));
  free(text);
  return result;
}
static VWLink *vw_append(VWLink *previous, vw_ast_operation_t operation) {
  VWLink *link = vw_ast_arena_alloc(vw_arena, sizeof(*link));
  if (link) {
    link->previous = previous;
    link->operation = operation;
    link->count = 1 + (previous ? previous->count : 0);
  }
  return link;
}

/* A result is copied out by the C++ frontend before its arena is destroyed. */
int vw_parse_operations(const char *input, vw_ast_arena *arena,
                        vw_ast_operation_seq *operations, size_t *offset) {
  VWLink *link;
  const char *error;
  size_t i;
  *offset = 0;
  vw_arena = arena;
  link = parse_vw_abstract_program(input);
  error = parse_vw_abstract_program_error();
  vw_arena = NULL;
  if (error) *offset = (size_t)(error - input);
  if (!link || arena->error) return 0;
  *operations = vw_ast_operation_seq_new(arena, link->count - 1);
  if (arena->error) return 0;
  i = operations->length;
  for (link = link->previous; link; link = link->previous)
    operations->items[--i] = link->operation;
  return 1;
}
