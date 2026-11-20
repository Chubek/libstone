#include "internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum {
    TOK_END,
    TOK_CANVAS,
    TOK_LET,
    TOK_NAME,
    TOK_NUMBER,
    TOK_STRING,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_COMMA,
    TOK_EQUALS,
    TOK_SEMICOLON
} token_kind;

typedef struct {
    token_kind kind;
    mv_location loc;
    mv_slice text;
} token;

typedef struct {
    const char *source;
    size_t size;
    mv_location loc;
} lexer;

typedef struct {
    lexer lex;
    token current;
    mv_diagnostic *diag;
    mv_status status;
} parser;

enum { MAX_EXPR_DEPTH = 128 };

static bool at_end(const lexer *lex)
{
    return lex->loc.offset == lex->size;
}

static unsigned char peek(const lexer *lex)
{
    return at_end(lex) ? 0 : (unsigned char)lex->source[lex->loc.offset];
}

static void step(lexer *lex)
{
    unsigned char c = peek(lex);
    ++lex->loc.offset;
    /* Count CRLF as one line break while still counting both source bytes. */
    if (c == '\r' && peek(lex) == '\n')
        ++lex->loc.offset;
    if (c == '\r' || c == '\n') {
        ++lex->loc.line;
        lex->loc.column = 1;
    } else {
        ++lex->loc.column;
    }
}

static bool is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

static bool is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static bool is_name_start(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool fail(parser *p, mv_status status, mv_location loc, const char *message)
{
    p->status = mv_fail(p->diag, status, loc, message);
    return false;
}

static bool skip_trivia(parser *p)
{
    lexer *lex = &p->lex;
    for (;;) {
        while (is_space(peek(lex)))
            step(lex);
        if (lex->size - lex->loc.offset < 2 || peek(lex) != '/' ||
            lex->source[lex->loc.offset + 1] != '/')
            return true;
        step(lex);
        step(lex);
        while (!at_end(lex) && peek(lex) != '\n' && peek(lex) != '\r') {
            if (peek(lex) == 0)
                return fail(p, MV_STATUS_SYNTAX_ERROR, lex->loc, "NUL byte in source");
            step(lex);
        }
    }
}

static bool scan_digits(lexer *lex)
{
    size_t start = lex->loc.offset;
    while (is_digit(peek(lex)))
        step(lex);
    return lex->loc.offset != start;
}

static bool scan_number(parser *p)
{
    lexer *lex = &p->lex;
    bool has_digits;
    if (peek(lex) == '+' || peek(lex) == '-')
        step(lex);
    has_digits = scan_digits(lex);
    if (peek(lex) == '.') {
        step(lex);
        if (scan_digits(lex))
            has_digits = true;
    }
    if (!has_digits)
        return fail(p, MV_STATUS_SYNTAX_ERROR, p->current.loc,
                    "expected digits in decimal literal");
    if (peek(lex) == 'e' || peek(lex) == 'E') {
        step(lex);
        if (peek(lex) == '+' || peek(lex) == '-')
            step(lex);
        if (!scan_digits(lex))
            return fail(p, MV_STATUS_SYNTAX_ERROR, lex->loc,
                        "expected digits in decimal exponent");
    }
    p->current.kind = TOK_NUMBER;
    p->current.text.size = lex->loc.offset - p->current.loc.offset;
    return true;
}

static bool scan_string(parser *p)
{
    lexer *lex = &p->lex;
    size_t start;
    step(lex);
    start = lex->loc.offset;
    while (!at_end(lex)) {
        unsigned char c = peek(lex);
        if (c == '"') {
            p->current.kind = TOK_STRING;
            p->current.text = (mv_slice){lex->source + start, lex->loc.offset - start};
            step(lex);
            return true;
        }
        if (c == 0)
            return fail(p, MV_STATUS_SYNTAX_ERROR, lex->loc, "NUL byte in source");
        if (c < 0x20 || c > 0x7e || c == '\\')
            return fail(p, MV_STATUS_SYNTAX_ERROR, lex->loc,
                        "expected printable ASCII in string; escapes are not supported");
        step(lex);
    }
    return fail(p, MV_STATUS_SYNTAX_ERROR, p->current.loc, "unterminated string");
}

static bool advance(parser *p)
{
    lexer *lex = &p->lex;
    unsigned char c;
    if (!skip_trivia(p))
        return false;
    p->current = (token){.kind = TOK_END, .loc = lex->loc};
    if (at_end(lex))
        return true;
    p->current.text.data = lex->source + lex->loc.offset;
    c = peek(lex);
    if (is_name_start(c)) {
        do {
            step(lex);
            c = peek(lex);
        } while (is_name_start(c) || is_digit(c));
        p->current.text.size = lex->loc.offset - p->current.loc.offset;
        if (mv_slice_eq(p->current.text, "canvas"))
            p->current.kind = TOK_CANVAS;
        else if (mv_slice_eq(p->current.text, "let"))
            p->current.kind = TOK_LET;
        else
            p->current.kind = TOK_NAME;
        return true;
    }
    if (is_digit(c) || c == '+' || c == '-' || c == '.')
        return scan_number(p);
    if (c == '"')
        return scan_string(p);
    switch (c) {
    case '(': p->current.kind = TOK_LPAREN; break;
    case ')': p->current.kind = TOK_RPAREN; break;
    case '{': p->current.kind = TOK_LBRACE; break;
    case '}': p->current.kind = TOK_RBRACE; break;
    case ',': p->current.kind = TOK_COMMA; break;
    case '=': p->current.kind = TOK_EQUALS; break;
    case ';': p->current.kind = TOK_SEMICOLON; break;
    case 0:
        return fail(p, MV_STATUS_SYNTAX_ERROR, lex->loc, "NUL byte in source");
    default:
        return fail(p, MV_STATUS_SYNTAX_ERROR, lex->loc, "unexpected byte in source");
    }
    p->current.text.size = 1;
    step(lex);
    return true;
}

static bool take(parser *p, token_kind kind, const char *message)
{
    if (p->current.kind != kind)
        return fail(p, MV_STATUS_SYNTAX_ERROR, p->current.loc, message);
    return advance(p);
}

static void destroy_args(mv_expr **args, size_t count);

static void destroy_expr(mv_expr *expr)
{
    if (expr) {
        destroy_args(expr->args, expr->arg_count);
        free(expr);
    }
}

static void destroy_args(mv_expr **args, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        destroy_expr(args[i]);
    free(args);
}

void mv_ast_destroy(mv_ast *ast)
{
    if (!ast)
        return;
    destroy_expr(ast->width);
    destroy_expr(ast->height);
    for (size_t i = 0; i < ast->stmt_count; ++i)
        destroy_args(ast->stmts[i].args, ast->stmts[i].arg_count);
    free(ast->stmts);
    *ast = (mv_ast){0};
}

/* Called only when the array is full; the caller retains ownership on failure. */
static void *grow(parser *p, void *buffer, size_t *capacity, size_t item_size)
{
    size_t max = SIZE_MAX / item_size;
    size_t next;
    void *resized;
    if (*capacity == max) {
        fail(p, MV_STATUS_LIMIT_EXCEEDED, p->current.loc, "too many syntax nodes");
        return NULL;
    }
    if (*capacity == 0)
        next = max < 4 ? max : 4;
    else
        next = *capacity > max / 2 ? max : *capacity * 2;
    resized = realloc(buffer, next * item_size);
    if (!resized) {
        fail(p, MV_STATUS_OUT_OF_MEMORY, p->current.loc, "cannot allocate syntax nodes");
        return NULL;
    }
    *capacity = next;
    return resized;
}

static mv_expr *parse_expr(parser *p, size_t depth);

static bool parse_args(parser *p, size_t depth, mv_expr ***args, size_t *count)
{
    size_t capacity = 0;
    if (!take(p, TOK_LPAREN, "expected '('"))
        return false;
    if (p->current.kind != TOK_RPAREN) {
        for (;;) {
            mv_expr *arg;
            if (*count == capacity) {
                mv_expr **resized = grow(p, *args, &capacity, sizeof(**args));
                if (!resized)
                    return false;
                *args = resized;
            }
            arg = parse_expr(p, depth);
            if (!arg)
                return false;
            (*args)[(*count)++] = arg;
            if (p->current.kind != TOK_COMMA)
                break;
            if (!advance(p))
                return false;
        }
    }
    return take(p, TOK_RPAREN, "expected ',' or ')' after argument");
}

static bool parse_number(parser *p, mv_expr *expr)
{
    mv_status status = mv_parse_number(p->current.text, &expr->number);
    switch (status) {
    case MV_STATUS_OK:
        return true;
    case MV_STATUS_OUT_OF_MEMORY:
        return fail(p, status, expr->loc, "cannot allocate decimal conversion buffer");
    case MV_STATUS_LIMIT_EXCEEDED:
        return fail(p, status, expr->loc, "decimal literal is too long");
    default:
        return fail(p, status, expr->loc, "decimal literal is outside the representable range");
    }
}

static mv_expr *parse_expr(parser *p, size_t depth)
{
    mv_expr *expr;
    token start = p->current;
    if (depth == MAX_EXPR_DEPTH) {
        fail(p, MV_STATUS_LIMIT_EXCEEDED, start.loc, "expression nesting limit exceeded");
        return NULL;
    }
    if (start.kind != TOK_NUMBER && start.kind != TOK_STRING && start.kind != TOK_NAME) {
        fail(p, MV_STATUS_SYNTAX_ERROR, start.loc,
             "expected a number, string, name, or function call");
        return NULL;
    }
    expr = calloc(1, sizeof(*expr));
    if (!expr) {
        fail(p, MV_STATUS_OUT_OF_MEMORY, start.loc, "cannot allocate expression");
        return NULL;
    }
    expr->loc = start.loc;
    if (start.kind == TOK_NUMBER) {
        expr->kind = MV_EXPR_NUMBER;
        if (!parse_number(p, expr))
            goto error;
    } else {
        expr->kind = start.kind == TOK_NAME ? MV_EXPR_NAME : MV_EXPR_STRING;
        expr->text = start.text;
    }
    if (!advance(p))
        goto error;
    if (expr->kind == MV_EXPR_NAME && p->current.kind == TOK_LPAREN) {
        expr->kind = MV_EXPR_CALL;
        if (!parse_args(p, depth + 1, &expr->args, &expr->arg_count))
            goto error;
    }
    return expr;

error:
    destroy_expr(expr);
    return NULL;
}

static bool parse_stmt(parser *p, mv_stmt *stmt)
{
    stmt->loc = p->current.loc;
    if (p->current.kind == TOK_LET) {
        mv_expr *value;
        stmt->kind = MV_STMT_LET;
        if (!advance(p))
            return false;
        stmt->name = p->current.text;
        if (!take(p, TOK_NAME, "expected a name after 'let'") ||
            !take(p, TOK_EQUALS, "expected '=' after binding name"))
            return false;
        value = parse_expr(p, 0);
        if (!value)
            return false;
        stmt->args = malloc(sizeof(*stmt->args));
        if (!stmt->args) {
            destroy_expr(value);
            return fail(p, MV_STATUS_OUT_OF_MEMORY, stmt->loc, "cannot allocate binding");
        }
        stmt->args[0] = value;
        stmt->arg_count = 1;
    } else {
        stmt->kind = MV_STMT_CALL;
        stmt->name = p->current.text;
        if (!advance(p) || !parse_args(p, 0, &stmt->args, &stmt->arg_count))
            return false;
    }
    return take(p, TOK_SEMICOLON, "expected ';' after statement");
}

mv_status mv_parse(const char *source, size_t size, mv_ast *ast, mv_diagnostic *diag)
{
    parser p = {.lex = {source, size, {0, 1, 1}}, .diag = diag, .status = MV_STATUS_OK};
    size_t capacity = 0;
    if (!ast || (!source && size))
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, p.lex.loc, "invalid parser input");
    /* Even an all-newline or single-line source must leave room for the EOF location. */
    if (size == SIZE_MAX)
        return mv_fail(diag, MV_STATUS_LIMIT_EXCEEDED, p.lex.loc, "source is too large");
    if (!advance(&p) ||
        !take(&p, TOK_CANVAS, "expected 'canvas'") ||
        !take(&p, TOK_LPAREN, "expected '(' after 'canvas'"))
        goto error;
    ast->width = parse_expr(&p, 0);
    if (!ast->width || !take(&p, TOK_COMMA, "expected ',' after canvas width"))
        goto error;
    ast->height = parse_expr(&p, 0);
    if (!ast->height ||
        !take(&p, TOK_RPAREN, "expected ')' after canvas height") ||
        !take(&p, TOK_LBRACE, "expected '{' after canvas dimensions"))
        goto error;
    while (p.current.kind != TOK_RBRACE) {
        mv_stmt *stmt;
        if (p.current.kind != TOK_LET && p.current.kind != TOK_NAME) {
            fail(&p, MV_STATUS_SYNTAX_ERROR, p.current.loc, "expected 'let', a call, or '}'");
            goto error;
        }
        if (ast->stmt_count == capacity) {
            mv_stmt *resized = grow(&p, ast->stmts, &capacity, sizeof(*ast->stmts));
            if (!resized)
                goto error;
            ast->stmts = resized;
        }
        stmt = &ast->stmts[ast->stmt_count++];
        *stmt = (mv_stmt){0};
        if (!parse_stmt(&p, stmt))
            goto error;
    }
    if (!advance(&p))
        goto error;
    if (p.current.kind != TOK_END) {
        fail(&p, MV_STATUS_SYNTAX_ERROR, p.current.loc, "expected end of source after canvas");
        goto error;
    }
    return MV_STATUS_OK;

error:
    mv_ast_destroy(ast);
    return p.status;
}
