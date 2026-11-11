%{
#include "Parser.h"
#include "ArchMLAST.h"

/* All semantic values, including abandoned alternatives, belong to this arena.
 * The C++ entry point serializes calls to the generated Aurocks parser. */
static archml_ast_arena *archml_arena;
typedef struct ArchMLLink {
    archml_ast_expr_t value;
    struct ArchMLLink *next;
} ArchMLLink;
typedef struct ArchMLItems {
    ArchMLLink *first, *last;
    size_t length;
} ArchMLItems;
static void *archml_items(void);
static void *archml_append(void *, void *);
static void *archml_list(void *, size_t);
static void *archml_atom(char *, int, size_t);
static size_t archml_offset(GLRParser p);
%}
%%
%start archml_document
%skip /([ \t\r\n]+|;[^\r\n]*)+/

archml_document: expression { $$ = archml_ast_make_document(archml_arena, $1); };
expression:
      '(' items ')' { $$ = archml_list($2, archml_offset(save)); }
    | /"([^"\\\r\n]|\\[^\r\n])*"/ { $$ = archml_atom($1, 1, archml_offset(save)); }
    | /[^()"; \t\r\n]+/ { $$ = archml_atom($1, 0, archml_offset(save)); }
    ;
items:
      items expression { $$ = archml_append($1, $2); }
    | %empty { $$ = archml_items(); }
    ;
%%
static size_t archml_offset(GLRParser p) {
    glr_skip(&p);
    return (size_t)(p.s - p.begin);
}
static void *archml_items(void) {
    ArchMLItems *items = archml_ast_arena_alloc(archml_arena, sizeof(*items));
    if (items) memset(items, 0, sizeof(*items));
    return items;
}
static void *archml_append(void *collection, void *value) {
    ArchMLItems *items = collection;
    ArchMLLink *link;
    if (!items || !value) return NULL;
    link = archml_ast_arena_alloc(archml_arena, sizeof(*link));
    if (!link) return NULL;
    link->value = value;
    link->next = NULL;
    if (items->last) items->last->next = link;
    else items->first = link;
    items->last = link;
    ++items->length;
    return items;
}
static void *archml_list(void *collection, size_t offset) {
    ArchMLItems *items = collection;
    archml_ast_expr_seq seq;
    ArchMLLink *link;
    size_t i = 0;
    if (!items) return NULL;
    seq = archml_ast_expr_seq_new(archml_arena, items->length);
    if (seq.length != items->length) return NULL;
    for (link = items->first; link; link = link->next) seq.items[i++] = link->value;
    return archml_ast_List(archml_arena, seq, offset);
}
static void *archml_atom(char *text, int quoted, size_t offset) {
    archml_ast_string_t copy = archml_ast_string_copy(archml_arena, text, strlen(text));
    free(text);
    return quoted ? archml_ast_String(archml_arena, copy, offset)
                  : archml_ast_Symbol(archml_arena, copy, offset);
}
struct archml_syntax {
    archml_ast_arena arena;
    archml_ast_document_t document;
};
archml_syntax *archml_parse_syntax(const char *input, size_t *error) {
    archml_syntax *result = malloc(sizeof(*result));
    const char *at;
    *error = 0;
    if (!result) return NULL;
    archml_ast_arena_init(&result->arena);
    archml_arena = &result->arena;
    result->document = parse_archml_document(input);
    at = parse_archml_document_error();
    *error = at ? (size_t)(at - input) : 0;
    archml_arena = NULL;
    if (!result->document || result->arena.error) { archml_syntax_free(result); return NULL; }
    return result;
}
void archml_syntax_free(archml_syntax *syntax) {
    if (syntax) { archml_ast_arena_destroy(&syntax->arena); free(syntax); }
}
const archml_syntax_node *archml_syntax_root(const archml_syntax *syntax) { return syntax->document->root; }
int archml_syntax_kind(const archml_syntax_node *node) {
    return node->kind == archml_ast_List_kind ? ARCHML_LIST :
           node->kind == archml_ast_String_kind ? ARCHML_STRING : ARCHML_SYMBOL;
}
size_t archml_syntax_offset(const archml_syntax_node *node) { return node->offset; }
const char *archml_syntax_text(const archml_syntax_node *node, size_t *size) {
    archml_ast_string_t text = node->kind == archml_ast_String_kind ? node->v.String.spelling : node->v.Symbol.text;
    *size = text.length;
    return text.data;
}
size_t archml_syntax_count(const archml_syntax_node *node) { return node->v.List.items.length; }
const archml_syntax_node *archml_syntax_child(const archml_syntax_node *node, size_t i) { return node->v.List.items.items[i]; }
