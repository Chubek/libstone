%{
#include "RewriteBankSyntax.h"
#define GLR_MAX_DEPTH 512
static novo_rwb_arena *rwb_arena;
typedef struct RWBList { void *value; struct RWBList *previous; size_t count; } RWBList;
static void *rwb_push(void *, void *);
static void *rwb_text(char *);
static novo_rwb_string_t rwb_string(void *);
static novo_rwb_string_t rwb_word(const char *);
static size_t rwb_offset(GLRParser);
static novo_rwb_entry_seq rwb_entries(void *);
%}
%%
%start novo_rwb_document
%skip /([ \t\r\n]+|\/\/[^\r\n]*|#[^\r\n]*)+/
novo_rwb_document: /rewrite_bank\b/ identifier '{' entries '}'
  { $$ = novo_rwb_make_document(rwb_arena, rwb_string($2), rwb_entries($4)); };
entries: entries entry { $$ = rwb_push($1, $2); } | %empty { $$ = NULL; };
entry:
    /source\b/ quoted ';' { $$ = novo_rwb_Source(rwb_arena, rwb_string($2), rwb_offset(save)); }
  | /rewrite\b/ quoted ';' { $$ = novo_rwb_Rewrite(rwb_arena, rwb_string($2), rwb_offset(save)); }
  | /data\b/ identifier format quoted ';'
    { $$ = novo_rwb_Data(rwb_arena, rwb_string($2), rwb_string($3), rwb_string($4), rwb_offset(save)); }
  | /contract\b/ identifier format quoted ';'
    { $$ = novo_rwb_Contract(rwb_arena, rwb_string($2), rwb_string($3), rwb_string($4), rwb_offset(save)); }
  | /lua\b/ identifier phase quoted ';'
    { $$ = novo_rwb_Lua(rwb_arena, rwb_string($2), rwb_string($3), rwb_string($4), rwb_offset(save)); }
  | /guard\b/ identifier quoted ';'
    { $$ = novo_rwb_Guard(rwb_arena, rwb_word(""), rwb_string($2), rwb_string($3), rwb_offset(save)); }
  | /guard\b/ identifier identifier quoted ';'
    { $$ = novo_rwb_Guard(rwb_arena, rwb_string($2), rwb_string($3), rwb_string($4), rwb_offset(save)); };
format: /json\b/ { $$ = rwb_text($1); } | /yaml\b/ { $$ = rwb_text($1); };
phase: /evaluation\b/ { $$ = rwb_text($1); } | /runtime\b/ { $$ = rwb_text($1); };
identifier: /[A-Za-z_][A-Za-z_0-9]*/ { $$ = rwb_text($1); };
quoted: /"([^"\\\r\n]|\\["\\nrt])*"/ { $$ = rwb_text($1); };
%%
static size_t rwb_offset(GLRParser p) { glr_skip(&p); return (size_t)(p.s - p.begin); }
static void *rwb_text(char *text) {
    novo_rwb_string_t *value = novo_rwb_arena_alloc(rwb_arena, sizeof(*value));
    if (value) *value = novo_rwb_string_copy(rwb_arena, text, strlen(text));
    aurocks_free(text); return value;
}
static novo_rwb_string_t rwb_string(void *value) { return value ? *(novo_rwb_string_t *)value : novo_rwb_string_view("", 0); }
static novo_rwb_string_t rwb_word(const char *value) { return novo_rwb_string_view(value, strlen(value)); }
static void *rwb_push(void *previous, void *value) {
    RWBList *list = novo_rwb_arena_alloc(rwb_arena, sizeof(*list));
    if (!list) return NULL;
    list->previous = previous; list->value = value;
    list->count = previous ? ((RWBList *)previous)->count + 1 : 1; return list;
}
static novo_rwb_entry_seq rwb_entries(void *value) {
    RWBList *list = value; size_t count = list ? list->count : 0;
    novo_rwb_entry_seq seq = novo_rwb_entry_seq_new(rwb_arena, count);
    if (seq.length != count) return seq;
    for (; list; list = list->previous) seq.items[--count] = list->value;
    return seq;
}
struct novo_rwb_syntax { novo_rwb_arena arena; novo_rwb_document_t document; };
novo_rwb_syntax *novo_rwb_parse_syntax(const char *input, size_t *error) {
    novo_rwb_syntax *syntax = malloc(sizeof(*syntax));
    GLRParser parser; void *document = NULL; const char *at; int accepted;
    *error = 0; if (!syntax) return NULL;
    novo_rwb_arena_init(&syntax->arena); rwb_arena = &syntax->arena;
    parser.begin = input; parser.s = input; parser.end = input + strlen(input); parser.error = NULL;
    glr_error_pos = NULL;
    accepted = glr_novo_rwb_document(&parser, &document); glr_skip(&parser);
    syntax->document = document;
    at = glr_error_pos && glr_error_pos > parser.s ? glr_error_pos : parser.s;
    *error = (size_t)(at - input); rwb_arena = NULL;
    if (!accepted || parser.s != parser.end || !document || syntax->arena.error) {
        novo_rwb_syntax_free(syntax); return NULL;
    }
    return syntax;
}
novo_rwb_document_t novo_rwb_syntax_document(const novo_rwb_syntax *syntax) { return syntax->document; }
void novo_rwb_syntax_free(novo_rwb_syntax *syntax) {
    if (syntax) { novo_rwb_arena_destroy(&syntax->arena); free(syntax); }
}
