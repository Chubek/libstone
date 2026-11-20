#include "internal.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void check_location(mv_location loc, size_t offset, size_t line, size_t column)
{
    CHECK(loc.offset == offset);
    CHECK(loc.line == line);
    CHECK(loc.column == column);
}

static void check_empty(const mv_ast *ast)
{
    CHECK(!ast->width && !ast->height && !ast->stmts && !ast->stmt_count);
}

static void test_grammar(void)
{
    static const char source[] =
        "// prolog\r\n"
        "\tcanvas(+1.25e+2,.5) {\r\n"
        "let _value2 = combine(1., -2E-3, name, \"#f00\", other(), \"\");\r"
        "unrecognized(_value2, \"not a color //\", letMe); // inline\n"
        "empty();\n"
        "let canvas2 = -0e999999;\n"
        "last(0);\n"
        "} // no newline";
    const char *let = strstr(source, "let ");
    const char *call = strstr(source, "unrecognized");
    const char *combine = strstr(source, "combine");
    const char *color = strstr(source, "#f00");
    mv_ast ast = {0};
    mv_stmt *stmt;
    mv_expr *value;
    CHECK(mv_parse(source, sizeof(source) - 1, &ast, NULL) == MV_STATUS_OK);
    CHECK(ast.width->kind == MV_EXPR_NUMBER && ast.width->number == 125);
    CHECK(ast.height->kind == MV_EXPR_NUMBER && ast.height->number == .5);
    check_location(ast.width->loc, 19, 2, 9);
    CHECK(ast.stmt_count == 5);

    stmt = &ast.stmts[0];
    check_location(stmt->loc, (size_t)(let - source), 3, 1);
    CHECK(stmt->kind == MV_STMT_LET && stmt->arg_count == 1);
    CHECK(mv_slice_eq(stmt->name, "_value2") && stmt->name.data == let + 4);
    value = stmt->args[0];
    check_location(value->loc, (size_t)(combine - source), 3, 15);
    CHECK(value->kind == MV_EXPR_CALL && mv_slice_eq(value->text, "combine"));
    CHECK(value->arg_count == 6);
    CHECK(value->args[0]->kind == MV_EXPR_NUMBER && value->args[0]->number == 1);
    CHECK(value->args[1]->kind == MV_EXPR_NUMBER && value->args[1]->number == -.002);
    CHECK(value->args[2]->kind == MV_EXPR_NAME && mv_slice_eq(value->args[2]->text, "name"));
    CHECK(value->args[3]->kind == MV_EXPR_STRING && mv_slice_eq(value->args[3]->text, "#f00"));
    CHECK(value->args[3]->text.data == color && value->args[3]->loc.offset == (size_t)(color - source - 1));
    CHECK(value->args[4]->kind == MV_EXPR_CALL && mv_slice_eq(value->args[4]->text, "other"));
    CHECK(!value->args[4]->arg_count);
    CHECK(value->args[5]->kind == MV_EXPR_STRING && !value->args[5]->text.size);

    stmt = &ast.stmts[1];
    check_location(stmt->loc, (size_t)(call - source), 4, 1);
    CHECK(stmt->kind == MV_STMT_CALL && mv_slice_eq(stmt->name, "unrecognized"));
    CHECK(stmt->arg_count == 3);
    CHECK(stmt->args[1]->kind == MV_EXPR_STRING &&
          mv_slice_eq(stmt->args[1]->text, "not a color //"));
    CHECK(mv_slice_eq(stmt->args[2]->text, "letMe"));
    CHECK(ast.stmts[2].kind == MV_STMT_CALL && !ast.stmts[2].arg_count);
    CHECK(mv_slice_eq(ast.stmts[3].name, "canvas2"));
    CHECK(ast.stmts[3].args[0]->number == 0 && signbit(ast.stmts[3].args[0]->number));
    CHECK(ast.stmts[4].arg_count == 1);
    mv_ast_destroy(&ast);
    check_empty(&ast);
    mv_ast_destroy(&ast);
}

static void expect_failure(const char *source, size_t size, mv_status status,
                           size_t offset, size_t line, size_t column)
{
    mv_ast ast = {0};
    mv_diagnostic diag = {0};
    if (mv_parse(source, size, &ast, &diag) != status ||
        diag.offset != offset || diag.line != line || diag.column != column) {
        fprintf(stderr, "expected status %d at %zu:%zu (offset %zu), got %zu:%zu "
                "(offset %zu): %s\n", (int)status, line, column, offset,
                diag.line, diag.column, diag.offset, diag.message ? diag.message : "(none)");
        exit(EXIT_FAILURE);
    }
    CHECK(diag.message && *diag.message);
    check_empty(&ast);
    mv_ast_destroy(&ast);
}

static void test_invalid(void)
{
    const struct {
        const char *source;
        size_t offset;
    } cases[] = {
        {"", 0},
        {"Canvas(1,2){}", 0},
        {"canvas(1 2){}", 9},
        {"canvas(1,2){} extra()", 14},
        {"canvas(1,2){", 12},
        {"canvas(1,2){let x 2;}", 18},
        {"canvas(1,2){let x = 2}", 21},
        {"canvas(1,2){let canvas = 2;}", 16},
        {"canvas(let,2){}", 7},
        {"canvas(1,2){f(1,);}", 16},
        {"canvas(f(,1),2){}", 9},
        {"canvas(f(1 2),2){}", 11},
        {"canvas(1,2){f;}", 13},
        {"canvas(1.,.e1){}", 10},
        {"canvas(+,2){}", 7},
        {"canvas(- 1,2){}", 7},
        {"canvas(1e+,2){}", 10},
        {"canvas(1e,2){}", 9},
        {"canvas(1e-999999,2){}", 7},
        {"canvas(1e999999,2){}", 7},
        {"canvas(+inf,2){}", 7},
        {"canvas(0x1p2,2){}", 8},
        {"canvas(2_0,2){}", 8},
        {"canvas(1,2){f(\"a\\b\");}", 16},
        {"canvas(1,2){f(\"a\tb\");}", 16},
        {"canvas(1,2){f(\"\xC3\xA9\");}", 15},
        {"canvas(1,2){f(\"a);}", 14},
    };
    static const char nul[] = "canvas(1,2){}\0trailing";
    static const char nul_in_comment[] = "// x\0\ncanvas(1,2){}";
    static const char nul_in_string[] = "canvas(\"a\0b\",2){}";
    static const char crlf[] = "canvas(1,2){\r\n\tbad(,);\r\n}";
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        expect_failure(cases[i].source, strlen(cases[i].source), MV_STATUS_SYNTAX_ERROR,
                       cases[i].offset, 1, cases[i].offset + 1);
    }
    expect_failure(nul, sizeof(nul) - 1, MV_STATUS_SYNTAX_ERROR, 13, 1, 14);
    expect_failure(nul_in_comment, sizeof(nul_in_comment) - 1, MV_STATUS_SYNTAX_ERROR, 4, 1, 5);
    expect_failure(nul_in_string, sizeof(nul_in_string) - 1, MV_STATUS_SYNTAX_ERROR, 9, 1, 10);
    expect_failure(crlf, sizeof(crlf) - 1, MV_STATUS_SYNTAX_ERROR, 19, 2, 6);
}

static void test_bounded_input(void)
{
    /* A complete canvas needs no NUL terminator. Each shorter prefix is incomplete. */
    static const char source[] = "canvas(foo(1,2,3,4,5),\"abc\"){let x=foo(1,2);shape(x);}";
    static const char bytes[] = {'c','a','n','v','a','s','(','1',',','2',')','{','}'};
    mv_ast ast = {0};
    CHECK(mv_parse(bytes, sizeof(bytes), &ast, NULL) == MV_STATUS_OK);
    mv_ast_destroy(&ast);
    for (size_t size = 0; size < sizeof(source) - 1; ++size) {
        CHECK(mv_parse(source, size, &ast, NULL) == MV_STATUS_SYNTAX_ERROR);
        check_empty(&ast);
        mv_ast_destroy(&ast);
    }
}

static void test_nesting(void)
{
    static const char suffix[] = ",1){}";
    char source[7 + 128 * 3 + 1 + sizeof(suffix) - 1];
    mv_ast ast = {0};
    for (size_t layers = 127; layers <= 128; ++layers) {
        size_t size = 7;
        memcpy(source, "canvas(", size);
        for (size_t i = 0; i < layers; ++i) {
            source[size++] = 'f';
            source[size++] = '(';
        }
        source[size++] = '1';
        memset(source + size, ')', layers);
        size += layers;
        memcpy(source + size, suffix, sizeof(suffix) - 1);
        size += sizeof(suffix) - 1;
        if (layers == 127) {
            CHECK(mv_parse(source, size, &ast, NULL) == MV_STATUS_OK);
            mv_ast_destroy(&ast);
        } else {
            expect_failure(source, size, MV_STATUS_LIMIT_EXCEEDED, 7 + 2 * 128, 1, 8 + 2 * 128);
        }
    }
}

static void test_subnormal(void)
{
#if FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MIN_EXP == -1021
    static const char source[] = "canvas(5e-324,1){}";
    mv_ast ast = {0};
    if (DBL_TRUE_MIN < DBL_MIN) {
        CHECK(mv_parse(source, sizeof(source) - 1, &ast, NULL) == MV_STATUS_OK);
        CHECK(ast.width->number == DBL_TRUE_MIN);
        mv_ast_destroy(&ast);
    }
#endif
}

int main(void)
{
    test_grammar();
    test_invalid();
    test_bounded_input();
    test_nesting();
    test_subnormal();
    return EXIT_SUCCESS;
}
