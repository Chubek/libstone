%{
#include "Syntax.h"

/* Semantic values and persistent lists share one parse-owned arena. */
static novo_dl_arena *dl_arena;
typedef struct DLList {
    void *value;
    struct DLList *previous;
    size_t count;
} DLList;
static void *dl_push(void *, void *);
static void *dl_text(char *);
static novo_dl_string_t dl_string(void *);
static novo_dl_statement_seq dl_statements(void *);
static novo_dl_column_seq dl_columns(void *);
static novo_dl_term_seq dl_terms(void *);
static novo_dl_atom_seq dl_atoms(void *);
%}
%%
%start novo_dl_document
%skip /([ \t\r\n]+|\/\/[^\r\n]*|#[^\r\n]*)+/

novo_dl_document: statements { $$ = novo_dl_make_document(dl_arena, dl_statements($1)); };
statements:
      statements statement { $$ = dl_push($1, $2); }
    | %empty { $$ = NULL; }
    ;
statement:
      /\.decl\b/ name '(' columns ')' derived '.'
        { $$ = novo_dl_Decl(dl_arena, dl_string($2), dl_columns($4), $6 != NULL, $OFFSET); }
    | atom ':-' body '.' { $$ = novo_dl_Rule(dl_arena, $1, dl_atoms($3), $OFFSET); }
    | atom '.' { $$ = novo_dl_Fact(dl_arena, $1, $OFFSET); }
    ;
derived: 'derived' { $$ = (void *)1; } | %empty { $$ = NULL; };
columns: column_list { $$ = $1; } | %empty { $$ = NULL; };
column_list:
      column_list ',' column { $$ = dl_push($1, $3); }
    | column { $$ = dl_push(NULL, $1); }
    ;
column:
      name ':' identifier { $$ = novo_dl_make_column(dl_arena, dl_string($1), dl_string($3)); }
    | identifier { $$ = novo_dl_make_column(dl_arena, novo_dl_string_view("", 0), dl_string($1)); }
    ;
body: atom_list { $$ = $1; } | %empty { $$ = NULL; };
atom_list:
      atom_list ',' literal { $$ = dl_push($1, $3); }
    | literal { $$ = dl_push(NULL, $1); }
    ;
literal:
      '!' atom { novo_dl_atom_t a = $2; $$ = a ? novo_dl_make_atom(dl_arena, a->name, a->terms, true, $OFFSET) : NULL; }
    | atom { $$ = $1; }
    ;
atom: name '(' terms ')' { $$ = novo_dl_make_atom(dl_arena, dl_string($1), dl_terms($3), false, $OFFSET); };
terms: term_list { $$ = $1; } | %empty { $$ = NULL; };
term_list:
      term_list ',' term { $$ = dl_push($1, $3); }
    | term { $$ = dl_push(NULL, $1); }
    ;
term:
      identifier { novo_dl_string_t s = dl_string($1); $$ = s.length == 1 && s.data[0] == '_' ? novo_dl_Wildcard(dl_arena, $OFFSET) : novo_dl_Variable(dl_arena, s, $OFFSET); }
    | integer { $$ = novo_dl_Integer(dl_arena, dl_string($1), $OFFSET); }
    | quoted { $$ = novo_dl_Symbol(dl_arena, dl_string($1), $OFFSET); }
    | '?' quoted { $$ = novo_dl_Variable(dl_arena, dl_string($2), $OFFSET); }
    ;
name: identifier { $$ = $1; } | quoted { $$ = $1; };
identifier: /[A-Za-z_][A-Za-z_0-9]*/ { $$ = dl_text($1); };
integer: /-?[0-9]+/ { $$ = dl_text($1); };
quoted: /"([^"\\\r\n]|\\[^\r\n])*"/ { $$ = dl_text($1); };
%%
static void *dl_text(char *text) {
    novo_dl_string_t *value = novo_dl_arena_alloc(dl_arena, sizeof(*value));
    if (value) *value = novo_dl_string_copy(dl_arena, text, strlen(text));
    aurocks_free(text);
    return value;
}
static novo_dl_string_t dl_string(void *value) {
    return value ? *(novo_dl_string_t *)value : novo_dl_string_view("", 0);
}
static void *dl_push(void *previous, void *value) {
    DLList *list = novo_dl_arena_alloc(dl_arena, sizeof(*list));
    if (!list) return NULL;
    list->previous = previous;
    list->value = value;
    list->count = previous ? ((DLList *)previous)->count + 1 : 1;
    return list;
}
#define DL_SEQUENCE(helper, type) \
static novo_dl_##type##_seq helper(void *value) { \
    DLList *list = value; \
    size_t count = list ? list->count : 0; \
    novo_dl_##type##_seq seq = novo_dl_##type##_seq_new(dl_arena, count); \
    if (seq.length != count) return seq; \
    for (; list; list = list->previous) seq.items[--count] = list->value; \
    return seq; \
}
DL_SEQUENCE(dl_statements, statement)
DL_SEQUENCE(dl_columns, column)
DL_SEQUENCE(dl_terms, term)
DL_SEQUENCE(dl_atoms, atom)
#undef DL_SEQUENCE

struct novo_dl_syntax {
    novo_dl_arena arena;
    novo_dl_document_t document;
};
novo_dl_syntax *novo_dl_parse_syntax(const char *input, size_t *error) {
    novo_dl_syntax *syntax = malloc(sizeof(*syntax));
    const char *at;
    *error = 0;
    if (!syntax) return NULL;
    novo_dl_arena_init(&syntax->arena);
    dl_arena = &syntax->arena;
    syntax->document = parse_novo_dl_document(input);
    at = parse_novo_dl_document_error();
    if (at) *error = (size_t)(at - input);
    dl_arena = NULL;
    if (at || !syntax->document || syntax->arena.error) {
        novo_dl_syntax_free(syntax); return NULL;
    }
    return syntax;
}
novo_dl_document_t novo_dl_syntax_document(const novo_dl_syntax *syntax) { return syntax->document; }
void novo_dl_syntax_free(novo_dl_syntax *syntax) {
    if (syntax) { novo_dl_arena_destroy(&syntax->arena); free(syntax); }
}
