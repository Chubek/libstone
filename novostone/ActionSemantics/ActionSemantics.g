%{
#include "Syntax.h"
#define GLR_MAX_DEPTH 512
static novo_as_arena *as_arena;
typedef struct ASList { void *value; struct ASList *previous; size_t count; } ASList;
static void *as_push(void *, void *);
static void *as_text(char *);
static novo_as_string_t as_string(void *);
static novo_as_string_t as_word(const char *);
static size_t as_offset(GLRParser);
static novo_as_sort_seq as_sorts(void *);
static novo_as_constructor_seq as_constructors(void *);
static novo_as_field_seq as_fields(void *);
static novo_as_rule_seq as_rules(void *);
static novo_as_expr_seq as_operands(void *, void *);
%}
%%
%start novo_as_document
%skip /([ \t\r\n]+|\/\/[^\r\n]*|#[^\r\n]*)+/
novo_as_document: /language\b/ identifier '{' /start\b/ identifier ';' sorts rules '}'
  { $$ = novo_as_make_document(as_arena, as_string($2), as_string($5), as_sorts($7), as_rules($8)); };
sorts: sorts sort { $$ = as_push($1, $2); } | sort { $$ = as_push(NULL, $1); };
sort: /syntax\b/ identifier '=' constructors ';'
  { $$ = novo_as_make_sort(as_arena, as_string($2), as_constructors($4), as_offset(save)); };
constructors: constructors '|' constructor { $$ = as_push($1, $3); } | constructor { $$ = as_push(NULL, $1); };
constructor: identifier '(' fields ')' { $$ = novo_as_make_constructor(as_arena, as_string($1), as_fields($3), as_offset(save)); };
fields: field_list { $$ = $1; } | %empty { $$ = NULL; };
field_list: field_list ',' field { $$ = as_push($1, $3); } | field { $$ = as_push(NULL, $1); };
field: identifier ':' identifier { $$ = novo_as_make_field(as_arena, as_string($1), as_string($3), as_offset(save)); };
rules: rules rule { $$ = as_push($1, $2); } | rule { $$ = as_push(NULL, $1); };
rule: /rule\b/ identifier '=' action ';' { $$ = novo_as_make_rule(as_arena, as_string($2), $4, as_offset(save)); };
action:
    action /and\b/ /then\b/ primitive { $$ = novo_as_Combine(as_arena, as_word("and then"), $1, $4, as_offset(save)); }
  | action /then\b/ primitive { $$ = novo_as_Combine(as_arena, as_word("then"), $1, $3, as_offset(save)); }
  | action /hence\b/ primitive { $$ = novo_as_Combine(as_arena, as_word("hence"), $1, $3, as_offset(save)); }
  | action /otherwise\b/ primitive { $$ = novo_as_Combine(as_arena, as_word("otherwise"), $1, $3, as_offset(save)); }
  | action /trapping\b/ primitive { $$ = novo_as_Combine(as_arena, as_word("trapping"), $1, $3, as_offset(save)); }
  | primitive { $$ = $1; };
primitive:
    /furthermore\b/ primitive { $$ = novo_as_Furthermore(as_arena, $2, as_offset(save)); }
  | /give\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("give"), as_operands($2, NULL), as_offset(save)); }
  | /check\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("check"), as_operands($2, NULL), as_offset(save)); }
  | /escape\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("escape"), as_operands($2, NULL), as_offset(save)); }
  | /bind\b/ expr /to\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("bind"), as_operands($2, $4), as_offset(save)); }
  | /store\b/ expr /in\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("store"), as_operands($2, $4), as_offset(save)); }
  | /allocate\b/ /cell\b/ { $$ = novo_as_Primitive(as_arena, as_word("allocate"), as_operands(NULL, NULL), as_offset(save)); }
  | /complete\b/ { $$ = novo_as_Primitive(as_arena, as_word("complete"), as_operands(NULL, NULL), as_offset(save)); }
  | /fail\b/ { $$ = novo_as_Primitive(as_arena, as_word("fail"), as_operands(NULL, NULL), as_offset(save)); }
  | /evaluate\b/ '(' identifier ')' { $$ = novo_as_Evaluate(as_arena, as_string($3), as_offset(save)); }
  | '(' action ')' { $$ = $2; };
expr: sum '==' sum { $$ = novo_as_Binary(as_arena, as_word("=="), $1, $3, as_offset(save)); }
  | sum '<' sum { $$ = novo_as_Binary(as_arena, as_word("<"), $1, $3, as_offset(save)); }
  | sum { $$ = $1; };
sum: sum '+' product { $$ = novo_as_Binary(as_arena, as_word("+"), $1, $3, as_offset(save)); }
  | sum '-' product { $$ = novo_as_Binary(as_arena, as_word("-"), $1, $3, as_offset(save)); }
  | product { $$ = $1; };
product: product '*' atom { $$ = novo_as_Binary(as_arena, as_word("*"), $1, $3, as_offset(save)); }
  | product '/' atom { $$ = novo_as_Binary(as_arena, as_word("/"), $1, $3, as_offset(save)); }
  | atom { $$ = $1; };
atom: integer { $$ = novo_as_Integer(as_arena, as_string($1), as_offset(save)); }
  | /given\b/ '(' integer ')' { $$ = novo_as_Unary(as_arena, as_word("given"), novo_as_Integer(as_arena, as_string($3), as_offset(save)), as_offset(save)); }
  | /bound\b/ '(' expr ')' { $$ = novo_as_Unary(as_arena, as_word("bound"), $3, as_offset(save)); }
  | /contents\b/ '(' expr ')' { $$ = novo_as_Unary(as_arena, as_word("contents"), $3, as_offset(save)); }
  | quoted { $$ = novo_as_Token(as_arena, as_string($1), as_offset(save)); }
  | identifier { $$ = novo_as_Field(as_arena, as_string($1), as_offset(save)); }
  | '(' expr ')' { $$ = $2; };
identifier: /[A-Za-z_][A-Za-z_0-9]*/ { $$ = as_text($1); };
integer: /-?[0-9]+/ { $$ = as_text($1); };
quoted: /"[A-Za-z_][A-Za-z_0-9]*"/ { $$ = as_text($1); };
%%
static size_t as_offset(GLRParser p) { glr_skip(&p); return (size_t)(p.s - p.begin); }
static void *as_text(char *text) {
    novo_as_string_t *value = novo_as_arena_alloc(as_arena, sizeof(*value));
    if (value) *value = novo_as_string_copy(as_arena, text, strlen(text));
    aurocks_free(text); return value;
}
static novo_as_string_t as_string(void *value) { return value ? *(novo_as_string_t *)value : novo_as_string_view("", 0); }
static novo_as_string_t as_word(const char *value) { return novo_as_string_view(value, strlen(value)); }
static void *as_push(void *previous, void *value) {
    ASList *list = novo_as_arena_alloc(as_arena, sizeof(*list));
    if (!list) return NULL;
    list->previous = previous; list->value = value;
    list->count = previous ? ((ASList *)previous)->count + 1 : 1; return list;
}
#define AS_SEQUENCE(helper, type) \
static novo_as_##type##_seq helper(void *value) { \
    ASList *list = value; size_t count = list ? list->count : 0; \
    novo_as_##type##_seq seq = novo_as_##type##_seq_new(as_arena, count); \
    if (seq.length != count) return seq; \
    for (; list; list = list->previous) seq.items[--count] = list->value; \
    return seq; \
}
AS_SEQUENCE(as_sorts, sort)
AS_SEQUENCE(as_constructors, constructor)
AS_SEQUENCE(as_fields, field)
AS_SEQUENCE(as_rules, rule)
#undef AS_SEQUENCE
static novo_as_expr_seq as_operands(void *a, void *b) {
    novo_as_expr_seq seq = novo_as_expr_seq_new(as_arena, a ? (b ? 2 : 1) : 0);
    if (seq.length) seq.items[0] = a;
    if (seq.length == 2) seq.items[1] = b;
    return seq;
}
struct novo_as_syntax { novo_as_arena arena; novo_as_document_t document; };
novo_as_syntax *novo_as_parse_syntax(const char *input, size_t *error) {
    novo_as_syntax *syntax = malloc(sizeof(*syntax));
    GLRParser parser; void *document = NULL; const char *at; int accepted;
    *error = 0; if (!syntax) return NULL;
    novo_as_arena_init(&syntax->arena); as_arena = &syntax->arena;
    parser.begin = input; parser.s = input; parser.end = input + strlen(input); parser.error = NULL;
    glr_error_pos = NULL;
    accepted = glr_novo_as_document(&parser, &document); glr_skip(&parser);
    syntax->document = document;
    at = glr_error_pos && glr_error_pos > parser.s ? glr_error_pos : parser.s;
    *error = (size_t)(at - input); as_arena = NULL;
    if (!accepted || parser.s != parser.end || !document || syntax->arena.error) {
        novo_as_syntax_free(syntax); return NULL;
    }
    return syntax;
}
novo_as_document_t novo_as_syntax_document(const novo_as_syntax *syntax) { return syntax->document; }
void novo_as_syntax_free(novo_as_syntax *syntax) {
    if (syntax) { novo_as_arena_destroy(&syntax->arena); free(syntax); }
}
