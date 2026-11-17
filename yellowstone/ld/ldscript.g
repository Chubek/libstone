%{
#include "Expression.h"
#include "ldscript_ast.h"

/* This pointer is used only while the C++ wrapper holds the parser mutex. */
static qldexpr_arena *arena;
static size_t nodes;
static qldexpr_string_t text(const char *s) {
  return qldexpr_string_copy(arena, s, strlen(s));
}
static void *bounded(void *node) {
  if (++nodes > 4096) qldexpr_set_error(arena, QLDEXPR_OVERFLOW);
  return node;
}
static void *number(const char *s) { return bounded(qldexpr_Number(arena, text(s))); }
static void *symbol(const char *s) { return bounded(qldexpr_Symbol(arena, text(s))); }
static void *unary(const char *op, void *a) {
  return bounded(qldexpr_Unary(arena, text(op), a));
}
static void *binary(const char *op, void *a, void *b) {
  return bounded(qldexpr_Binary(arena, text(op), a, b));
}
/* Use Call's typed sequence as an arena-owned argument accumulator. */
static void *arguments(void *previous, void *value) {
  qldexpr_expr_t prev = previous;
  size_t n = prev ? prev->v.Call.arguments.length : 0;
  qldexpr_expr_seq seq = qldexpr_expr_seq_new(arena, n + 1);
  if (!seq.items) return NULL;
  if (n) memcpy(seq.items, prev->v.Call.arguments.items, n * sizeof(*seq.items));
  seq.items[n] = value;
  return bounded(qldexpr_Call(arena, text(""), seq));
}
static void *call(const char *name, void *args) {
  qldexpr_expr_t a = args;
  qldexpr_expr_seq empty = {0, NULL};
  return bounded(qldexpr_Call(arena, text(name), a ? a->v.Call.arguments : empty));
}
static void *quoted_symbol(const char *s) {
  size_t n = strlen(s), j = 0;
  char *copy = qldexpr_arena_alloc(arena, n);
  if (!copy) return NULL;
  for (size_t i = 1; i + 1 < n; ++i) {
    if (s[i] == '\\' && i + 2 < n) ++i;
    copy[j++] = s[i];
  }
  copy[j] = 0;
  return symbol(copy);
}
%}
%%
%start qld_expr
%skip /([ \t\r\n\f\v]+|\/\*([^*]|\*+[^*\/])*\*+\/|\/\/[^\n]*|#[^\n]*)*/
qld_expr : logical_or conditional_tail {
  qldexpr_expr_t tail = $2;
  if (tail) { tail->v.Conditional.condition = $1; $$ = tail; }
  else $$ = $1;
} ;
conditional_tail : '?' qld_expr ':' qld_expr
  { $$ = bounded(qldexpr_Conditional(arena, qldexpr_Dot(arena), $2, $4)); }
  | %empty { $$ = NULL; } ;
logical_or : logical_or '||' logical_and { $$ = binary("||", $1, $3); }
           | logical_and { $$ = $1; } ;
logical_and : logical_and '&&' bit_or { $$ = binary("&&", $1, $3); }
            | bit_or { $$ = $1; } ;
bit_or : bit_or '|' bit_xor { $$ = binary("|", $1, $3); } | bit_xor { $$ = $1; } ;
bit_xor : bit_xor '^' bit_and { $$ = binary("^", $1, $3); } | bit_and { $$ = $1; } ;
bit_and : bit_and '&' equality { $$ = binary("&", $1, $3); } | equality { $$ = $1; } ;
equality : equality '==' relational { $$ = binary("==", $1, $3); }
         | equality '!=' relational { $$ = binary("!=", $1, $3); }
         | relational { $$ = $1; } ;
relational : relational '<=' shift { $$ = binary("<=", $1, $3); }
           | relational '>=' shift { $$ = binary(">=", $1, $3); }
           | relational '<' shift { $$ = binary("<", $1, $3); }
           | relational '>' shift { $$ = binary(">", $1, $3); }
           | shift { $$ = $1; } ;
shift : shift '<<' additive { $$ = binary("<<", $1, $3); }
      | shift '>>' additive { $$ = binary(">>", $1, $3); }
      | additive { $$ = $1; } ;
additive : additive '+' multiplicative { $$ = binary("+", $1, $3); }
         | additive '-' multiplicative { $$ = binary("-", $1, $3); }
         | multiplicative { $$ = $1; } ;
multiplicative : multiplicative '*' unary_expr { $$ = binary("*", $1, $3); }
               | multiplicative '/' unary_expr { $$ = binary("/", $1, $3); }
               | multiplicative '%' unary_expr { $$ = binary("%", $1, $3); }
               | unary_expr { $$ = $1; } ;
unary_expr : '-' unary_expr { $$ = unary("-", $2); }
           | '+' unary_expr { $$ = $2; }
           | '!' unary_expr { $$ = unary("!", $2); }
           | '~' unary_expr { $$ = unary("~", $2); }
           | primary { $$ = $1; } ;
primary : /[A-Za-z_.$][A-Za-z0-9_.$]*/ '(' args ')' { $$ = call($1, $3); aurocks_free($1); }
        | /[A-Za-z_.$][A-Za-z0-9_.$]*/ '(' ')' { $$ = call($1, NULL); aurocks_free($1); }
        | /[0-9][A-Za-z0-9]*/ { $$ = number($1); aurocks_free($1); }
        | /[A-Za-z_.$][A-Za-z0-9_.$]*/ { $$ = strcmp($1, ".") ? symbol($1) : bounded(qldexpr_Dot(arena)); aurocks_free($1); }
        | /"([^"\\]|\\.)*"/ { $$ = quoted_symbol($1); aurocks_free($1); }
        | /'([^'\\]|\\.)*'/ { $$ = quoted_symbol($1); aurocks_free($1); }
        | '(' qld_expr ')' { $$ = $2; } ;
args : args ',' qld_expr { $$ = arguments($1, $3); }
     | qld_expr { $$ = arguments(NULL, $1); } ;
%%
struct qld_expression {
  qldexpr_arena storage;
  qldexpr_expr_t root;
};
qld_expression *qld_expression_open(const char *input, size_t *consumed) {
  qld_expression *result = calloc(1, sizeof(*result));
  void *root = NULL;
  GLRParser p;
  *consumed = 0;
  if (!result) return NULL;
  arena = &result->storage;
  nodes = 0;
  glr_depth = 0;
  glr_error_pos = NULL;
  p.begin = p.s = input; p.end = input + strlen(input); p.error = NULL;
  if (!glr_qld_expr(&p, &root) || !root || arena->error != QLDEXPR_OK) {
    qld_expression_close(result);
    arena = NULL;
    return NULL;
  }
  result->root = root;
  *consumed = (size_t)(p.s - input);
  arena = NULL;
  return result;
}
const void *qld_expression_root(const qld_expression *e) { return e->root; }
void qld_expression_close(qld_expression *e) {
  if (e) { qldexpr_arena_destroy(&e->storage); free(e); }
}
qld_expression_view qld_expression_inspect(const void *pointer) {
  const struct qldexpr_expr_node *e = pointer;
  qld_expression_view v = {0};
  qldexpr_string_t s = {0, NULL};
  switch (e->kind) {
  case qldexpr_Number_kind: v.kind = QLD_NUMBER; s = e->v.Number.spelling; break;
  case qldexpr_Symbol_kind: v.kind = QLD_SYMBOL; s = e->v.Symbol.name; break;
  case qldexpr_Dot_kind: v.kind = QLD_DOT; break;
  case qldexpr_Unary_kind:
    v.kind = QLD_UNARY; s = e->v.Unary.op; v.first = e->v.Unary.operand; break;
  case qldexpr_Binary_kind:
    v.kind = QLD_BINARY; s = e->v.Binary.op;
    v.first = e->v.Binary.left; v.second = e->v.Binary.right; break;
  case qldexpr_Conditional_kind:
    v.kind = QLD_CONDITIONAL; v.first = e->v.Conditional.condition;
    v.second = e->v.Conditional.yes; v.third = e->v.Conditional.no; break;
  case qldexpr_Call_kind:
    v.kind = QLD_CALL; s = e->v.Call.name; v.count = e->v.Call.arguments.length; break;
  }
  v.text = s.data; v.length = s.length;
  return v;
}
const void *qld_expression_argument(const void *pointer, size_t i) {
  const struct qldexpr_expr_node *e = pointer;
  return e->v.Call.arguments.items[i];
}
