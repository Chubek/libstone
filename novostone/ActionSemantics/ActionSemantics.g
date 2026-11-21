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
static novo_as_expr_seq as_expressions(void *);
static novo_as_binding_seq as_bindings(void *);
static novo_as_data_sort_seq as_data_sorts(void *);
static novo_as_variant_seq as_variants(void *);
static novo_as_data_type_seq as_data_types(void *);
static novo_as_arm_seq as_arms(void *);
static novo_as_pattern_seq as_patterns(void *);
static novo_as_rewrite_set_seq as_rewrites(void *);
static novo_as_bank_ref_seq as_banks(void *);
%}
%%
%start novo_as_document
%skip /([ \t\r\n]+|\/\/[^\r\n]*|#[^\r\n]*)+/
novo_as_document: /language\b/ identifier '{' /start\b/ identifier ';' sorts data_decls rewrite_decls rules '}'
  { $$ = novo_as_make_document(as_arena, as_string($2), as_string($5), as_sorts($7), as_data_sorts($8), as_rewrites($9), as_rules($10)); };
sorts: sorts sort { $$ = as_push($1, $2); } | sort { $$ = as_push(NULL, $1); };
sort: /syntax\b/ identifier '=' constructors ';'
  { $$ = novo_as_make_sort(as_arena, as_string($2), as_constructors($4), as_offset(save)); };
constructors: constructors '|' constructor { $$ = as_push($1, $3); } | constructor { $$ = as_push(NULL, $1); };
constructor: identifier '(' fields ')' { $$ = novo_as_make_constructor(as_arena, as_string($1), as_fields($3), as_offset(save)); };
fields: field_list { $$ = $1; } | %empty { $$ = NULL; };
field_list: field_list ',' field { $$ = as_push($1, $3); } | field { $$ = as_push(NULL, $1); };
field: identifier ':' identifier { $$ = novo_as_make_field(as_arena, as_string($1), as_string($3), as_offset(save)); };
data_decls: data_decls data_decl { $$ = as_push($1, $2); } | %empty { $$ = NULL; };
data_decl: /data\b/ identifier '=' variants ';'
  { $$ = novo_as_make_data_sort(as_arena, as_string($2), as_variants($4), as_offset(save)); };
variants: variants '|' variant { $$ = as_push($1, $3); } | variant { $$ = as_push(NULL, $1); };
variant: identifier '(' data_fields ')'
  { $$ = novo_as_make_variant(as_arena, as_string($1), as_data_types($3), as_offset(save)); };
data_fields: data_field_list { $$ = $1; } | %empty { $$ = NULL; };
data_field_list: data_field_list ',' data_field { $$ = as_push($1, $3); } | data_field { $$ = as_push(NULL, $1); };
data_field: identifier { $$ = novo_as_make_data_type(as_arena, as_string($1), as_offset(save)); };
rules: rules rule { $$ = as_push($1, $2); } | rule { $$ = as_push(NULL, $1); };
rewrite_decls: rewrite_decls rewrite_decl { $$ = as_push($1, $2); } | %empty { $$ = NULL; };
rewrite_decl: /rewrite\b/ identifier '=' '{' arms '}' ';'
  { $$ = novo_as_make_rewrite_set(as_arena, as_string($2), as_arms($5), as_offset(save)); };
rule: /rule\b/ identifier '=' action ';' { $$ = novo_as_make_rule(as_arena, as_string($2), $4, as_offset(save)); };
action:
    action /and\b/ /then\b/ primitive { $$ = novo_as_Combine(as_arena, as_word("and then"), $1, $4, as_offset(save)); }
  | action /then\b/ primitive { $$ = novo_as_Combine(as_arena, as_word("then"), $1, $3, as_offset(save)); }
  | action /hence\b/ primitive { $$ = novo_as_Combine(as_arena, as_word("hence"), $1, $3, as_offset(save)); }
  | action /otherwise\b/ primitive { $$ = novo_as_Combine(as_arena, as_word("otherwise"), $1, $3, as_offset(save)); }
  | action /trapping\b/ primitive { $$ = novo_as_Combine(as_arena, as_word("trapping"), $1, $3, as_offset(save)); }
  | primitive { $$ = $1; };
primitive:
    /rewrite\b/ rewrite_strategy expr /from\b/ /bank\b/ '(' bank_names ')'
      { $$ = novo_as_Rewrite(as_arena, as_string($2), $3, as_banks($7), as_arms(NULL), as_offset(save)); }
  | rewrite_strategy expr /using\b/ '{' arms '}'
      { $$ = novo_as_Rewrite(as_arena, as_string($1), $2, as_banks(NULL), as_arms($5), as_offset(save)); }
  | /rewrite\b/ rewrite_strategy expr /using\b/ '{' arms '}'
      { $$ = novo_as_Rewrite(as_arena, as_string($2), $3, as_banks(NULL), as_arms($6), as_offset(save)); }
  | /match\b/ /term\b/ expr match_domain /with\b/ '{' arms '}'
      { $$ = novo_as_Match(as_arena, $3, $4 ? novo_as_data_type_some($4) : novo_as_data_type_none(), as_arms($7), as_offset(save)); }
  | /match\b/ /dag\b/ expr match_domain /with\b/ '{' arms '}'
      { $$ = novo_as_Match(as_arena, $3, $4 ? novo_as_data_type_some($4) : novo_as_data_type_none(), as_arms($7), as_offset(save)); }
  | /furthermore\b/ primitive { $$ = novo_as_Furthermore(as_arena, $2, as_offset(save)); }
  | /closure\b/ primitive { $$ = novo_as_Closure(as_arena, $2, as_offset(save)); }
  | /give\b/ /closure\b/ primitive { $$ = novo_as_Closure(as_arena, $3, as_offset(save)); }
  | /abstraction\b/ primitive { $$ = novo_as_Closure(as_arena, $2, as_offset(save)); }
  | /callcc\b/ expr { $$ = novo_as_CallCC(as_arena, $2, as_offset(save)); }
  | /resume\b/ expr /with\b/ expr { $$ = novo_as_Resume(as_arena, $2, $4, as_offset(save)); }
  | /unfolding\b/ primitive { $$ = novo_as_Unfolding(as_arena, $2, as_offset(save)); }
  | /unfold\b/ { $$ = novo_as_Unfold(as_arena, as_offset(save)); }
  | /tailunfold\b/ { $$ = novo_as_TailUnfold(as_arena, as_offset(save)); }
  | /generator\b/ primitive { $$ = novo_as_Generator(as_arena, $2, as_offset(save)); }
  | /yield_value\b/ expr { $$ = novo_as_YieldValue(as_arena, $2, as_offset(save)); }
  | /next\b/ expr { $$ = novo_as_Next(as_arena, $2, as_offset(save)); }
  | /close\b/ expr { $$ = novo_as_Close(as_arena, $2, as_offset(save)); }
  | /if\b/ expr /then\b/ primitive /else\b/ primitive
      { $$ = novo_as_Conditional(as_arena, $2, $4, $6, as_offset(save)); }
  | /while\b/ expr /do\b/ primitive { $$ = novo_as_While(as_arena, $2, $4, as_offset(save)); }
  | /break\b/ { $$ = novo_as_Break(as_arena, as_offset(save)); }
  | /continue\b/ { $$ = novo_as_Continue(as_arena, as_offset(save)); }
  | /construct\b/ identifier args { $$ = novo_as_Construct(as_arena, as_string($2), as_expressions($3), as_offset(save)); }
  | /deconstruct\b/ expr { $$ = novo_as_Deconstruct(as_arena, $2, as_offset(save)); }
  | /match\b/ expr match_domain /with\b/ '{' arms '}'
    { $$ = novo_as_Match(as_arena, $2, $3 ? novo_as_data_type_some($3) : novo_as_data_type_none(), as_arms($6), as_offset(save)); }
  | /enact\b/ expr { $$ = novo_as_Enact(as_arena, $2, as_offset(save)); }
  | /apply\b/ expr /to\b/ args { $$ = novo_as_Apply(as_arena, $2, as_expressions($4), as_offset(save)); }
  | /tailcall\b/ expr /to\b/ args { $$ = novo_as_TailCall(as_arena, $2, as_expressions($4), as_offset(save)); }
  | /letrec\b/ letrecdefs /in\b/ primitive { $$ = novo_as_LetRec(as_arena, as_bindings($2), $4, as_offset(save)); }
  | /return\b/ expr { $$ = novo_as_Return(as_arena, $2, as_offset(save)); }
  | /give\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("give"), as_operands($2, NULL), as_offset(save)); }
  | /check\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("check"), as_operands($2, NULL), as_offset(save)); }
  | /escape\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("escape"), as_operands($2, NULL), as_offset(save)); }
  | /bind\b/ expr /to\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("bind"), as_operands($2, $4), as_offset(save)); }
  | /store\b/ expr /in\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("store"), as_operands($2, $4), as_offset(save)); }
  | /put\b/ expr /in\b/ expr { $$ = novo_as_Primitive(as_arena, as_word("put"), as_operands($2, $4), as_offset(save)); }
  | /allocate\b/ /cell\b/ { $$ = novo_as_Primitive(as_arena, as_word("allocate"), as_operands(NULL, NULL), as_offset(save)); }
  | /complete\b/ { $$ = novo_as_Primitive(as_arena, as_word("complete"), as_operands(NULL, NULL), as_offset(save)); }
  | /fail\b/ { $$ = novo_as_Primitive(as_arena, as_word("fail"), as_operands(NULL, NULL), as_offset(save)); }
  | /evaluate\b/ '(' identifier ')' { $$ = novo_as_Evaluate(as_arena, as_string($3), as_offset(save)); }
  | '(' action ')' { $$ = $2; };
expr: sum '==' sum { $$ = novo_as_Binary(as_arena, as_word("=="), $1, $3, as_offset(save)); }
  | sum '<' sum { $$ = novo_as_Binary(as_arena, as_word("<"), $1, $3, as_offset(save)); }
  | sum { $$ = $1; };
rewrite_strategy: /reduce\b/ { $$ = as_text($1); }
  | /combine\b/ { $$ = as_text($1); }
  | /merge\b/ { $$ = as_text($1); }
  | /saturate\b/ { $$ = as_text($1); }
  | /fixedpoint\b/ { $$ = as_text($1); }
  | /worklist\b/ { $$ = as_text($1); }
  | /traverse\b/ traversal_order { $$ = $2; };
traversal_order: /topdown\b/ { $$ = as_text($1); }
  | /bottomup\b/ { $$ = as_text($1); }
  | /once\b/ { $$ = as_text($1); };
bank_names: bank_names ',' bank_name { $$ = as_push($1, $3); } | bank_name { $$ = as_push(NULL, $1); };
bank_name: identifier { $$ = novo_as_make_bank_ref(as_arena, as_string($1), as_offset(save)); }
  | quoted { $$ = novo_as_make_bank_ref(as_arena, as_string($1), as_offset(save)); };
args: '(' ')' { $$ = NULL; } | '(' arglist ')' { $$ = $2; };
match_domain: /of\b/ data_field { $$ = $2; } | %empty { $$ = NULL; };
arms: arms ',' arm { $$ = as_push($1, $3); } | arm { $$ = as_push(NULL, $1); };
arm: pattern guard '=>' action
  { $$ = novo_as_MatchArm(as_arena, $1, $2 ? novo_as_expr_some($2) : novo_as_expr_none(), $4, as_offset(save)); };
guard: /when\b/ expr { $$ = $2; } | %empty { $$ = NULL; };
pattern: pattern '|' as_pattern { $$ = novo_as_OrPattern(as_arena, $1, $3, as_offset(save)); }
  | as_pattern { $$ = $1; };
as_pattern: pattern_atom /as\b/ identifier { $$ = novo_as_AsPattern(as_arena, $1, as_string($3), as_offset(save)); }
  | pattern_atom { $$ = $1; };
pattern_atom:
    identifier '(' pattern_args ')' { $$ = novo_as_VariantPattern(as_arena, as_string($1), as_patterns($3), as_offset(save)); }
  | /_\b/ { $$ = novo_as_Wildcard(as_arena, as_offset(save)); }
  | identifier { $$ = novo_as_Capture(as_arena, as_string($1), as_offset(save)); }
  | integer { $$ = novo_as_IntPattern(as_arena, as_string($1), as_offset(save)); }
  | quoted { $$ = novo_as_TokenPattern(as_arena, as_string($1), as_offset(save)); }
  | '(' pattern ')' { $$ = $2; };
pattern_args: pattern_list { $$ = $1; } | %empty { $$ = NULL; };
pattern_list: pattern_list ',' pattern { $$ = as_push($1, $3); } | pattern { $$ = as_push(NULL, $1); };
arglist: arglist ',' expr { $$ = as_push($1, $3); } | expr { $$ = as_push(NULL, $1); };
letrecdefs: letrecdefs ',' letrecdef { $$ = as_push($1, $3); } | letrecdef { $$ = as_push(NULL, $1); };
letrecdef: quoted '=' /closure\b/ primitive
  { $$ = novo_as_Binding(as_arena, as_string($1), novo_as_Closure(as_arena, $4, as_offset(save)), as_offset(save)); };
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
  | /arity\b/ '(' expr ')' { $$ = novo_as_Unary(as_arena, as_word("arity"), $3, as_offset(save)); }
  | /component\b/ '(' expr ',' expr ')' { $$ = novo_as_Binary(as_arena, as_word("component"), $3, $5, as_offset(save)); }
  | /equal\b/ '(' expr ',' expr ')' { $$ = novo_as_Binary(as_arena, as_word("equal"), $3, $5, as_offset(save)); }
  | /identical\b/ '(' expr ',' expr ')' { $$ = novo_as_Binary(as_arena, as_word("identical"), $3, $5, as_offset(save)); }
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
AS_SEQUENCE(as_expressions, expr)
AS_SEQUENCE(as_bindings, binding)
AS_SEQUENCE(as_data_sorts, data_sort)
AS_SEQUENCE(as_variants, variant)
AS_SEQUENCE(as_data_types, data_type)
AS_SEQUENCE(as_arms, arm)
AS_SEQUENCE(as_patterns, pattern)
AS_SEQUENCE(as_rewrites, rewrite_set)
AS_SEQUENCE(as_banks, bank_ref)
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
