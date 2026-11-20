#include "internal.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do {                                            \
    if (!(condition)) {                                                  \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,  \
                #condition);                                             \
        exit(EXIT_FAILURE);                                              \
    }                                                                    \
} while (0)

static void expect_location(mv_diagnostic diag, mv_location expected)
{
    CHECK(diag.offset == expected.offset);
    CHECK(diag.line == expected.line);
    CHECK(diag.column == expected.column);
    CHECK(diag.message != NULL);
}

static void expect_semantic_failure(const char *source, size_t offset, const char *message)
{
    mv_diagnostic diag = {0};
    mv_ast ast = {0};
    mv_scene scene = {0};
    mv_location expected = {.offset = offset, .line = 1, .column = 1};
    for (size_t i = 0; i < offset; ++i) {
        if (source[i] == '\n') {
            ++expected.line;
            expected.column = 1;
        } else {
            ++expected.column;
        }
    }

    if (mv_parse(source, strlen(source), &ast, &diag) != MV_STATUS_OK) {
        fprintf(stderr, "parse failed at %zu:%zu: %s\n%s\n", diag.line, diag.column,
                diag.message ? diag.message : "(no diagnostic)", source);
        exit(EXIT_FAILURE);
    }
    mv_status status = mv_lower(&ast, &scene, &diag);
    if (status != MV_STATUS_SEMANTIC_ERROR || diag.offset != offset) {
        fprintf(stderr, "expected semantic failure at %zu:%zu, got %s at %zu:%zu: %s\n%s\n",
                expected.line, expected.column, mv_status_string(status), diag.line, diag.column,
                diag.message ? diag.message : "(no diagnostic)", source);
        exit(EXIT_FAILURE);
    }
    expect_location(diag, expected);
    CHECK(strstr(diag.message, message) != NULL);
    CHECK(scene.shapes == NULL && scene.shape_count == 0);
    CHECK(scene.width == 0.0 && scene.height == 0.0);
    mv_scene_destroy(&scene);
    mv_ast_destroy(&ast);
}

#define FAIL_AT(prefix, suffix, message) \
    expect_semantic_failure(prefix suffix, sizeof(prefix) - 1, message)

static void test_semantic_failures(void)
{
    FAIL_AT("canvas(", "w, 10) { let w = 10; }", "binding");
    FAIL_AT("canvas(10,\n  ", "-0) {}", "canvas dimension");
    FAIL_AT("canvas(", "2147483647.25, 10) {}", "canvas dimension");
    FAIL_AT("canvas(", "\"#000000\", 10) {}", "expected a number");
    FAIL_AT("canvas(10,10) {\n  let a = ", "b;\n  let b = 1;\n}", "binding");
    FAIL_AT("canvas(10,10) { let a = ", "a; }", "binding");
    FAIL_AT("canvas(10,10) {\n  let a = 1;\n  ", "let a = 2;\n}", "duplicate");
    FAIL_AT("canvas(10,10) { let x = ", "\"#12345\"; }", "hexadecimal");
    FAIL_AT("canvas(10,10) { let x = ", "\"#abcdef0\"; }", "hexadecimal");
    FAIL_AT("canvas(10,10) { let x = ", "read(1, 2); }", "unknown arithmetic");
    FAIL_AT("canvas(10,10) { let x = ", "add(1); }", "two arguments");
    FAIL_AT("canvas(10,10) { let x = add(1, ", "\"#ffffff\"); }", "expected a number");
    FAIL_AT("canvas(10,10) { let x = div(1, ", "0); }", "division by zero");
    FAIL_AT("canvas(10,10) {\n  let z = -0;\n  let x = div(1, ", "z);\n}", "division by zero");
    FAIL_AT("canvas(10,10) { let x = ", "mul(1e308, 10); }", "finite");
    FAIL_AT("canvas(10,10) { let x = ", "add(1e308, 1e308); }", "finite");
    FAIL_AT("canvas(10,10) { let x = add(0, ", "div(1e308, .1)); }", "finite");
    FAIL_AT("canvas(10,10) { ", "rotate(1); }", "unknown geometry");
    FAIL_AT("canvas(10,10) { ", "circle(0, 1, 2); }", "number of geometry arguments");
    FAIL_AT("canvas(10,10) { rect(-1, -2, ", "-3, 4, \"#000000\"); }", "nonnegative");
    FAIL_AT("canvas(10,10) { rect(-1, -2, 3, ", "-4, \"#000000\"); }", "nonnegative");
    FAIL_AT("canvas(10,10) { circle(0, -2, ", "-3, \"#000000\"); }", "nonnegative");
    FAIL_AT("canvas(10,10) { circle(0, 1, 2, ", "\"#00x000\"); }", "hexadecimal");
    FAIL_AT("canvas(10,10) { let x = 1; circle(0, 1, 2, ", "x); }", "expected a color");
    FAIL_AT("canvas(10,10) { circle(0, 1, 2, \"#ffffff\");\n  line(0, 1, 2, 3, \"#000000\", ",
            "-0);\n}", "positive");
}

static void test_lower_success(void)
{
    char source[] =
        "canvas(2147483647, add(12, 1)) {\n"
        "  let aa = -0;\n"
        "  let a = sub(2, 5);\n"
        "  let z = mul(a, add(1, 1));\n"
        "  let c = \"#aB09fE\";\n"
        "  let ink = c;\n"
        "  rect(z, aa, 0, 3, ink);\n"
        "  circle(aa, a, aa, c);\n"
        "  line(1e300, z, 1e-310, a, \"#010203\", div(3, 2));\n"
        "}";
    mv_ast ast = {0};
    mv_scene scene = {0};
    CHECK(mv_parse(source, sizeof(source) - 1, &ast, NULL) == MV_STATUS_OK);
    CHECK(mv_lower(&ast, &scene, NULL) == MV_STATUS_OK);
    mv_ast_destroy(&ast);
    memset(source, 0, sizeof(source));

    CHECK(mv_verify(&scene, NULL) == MV_STATUS_OK);
    CHECK(scene.width == MV_MAX_CANVAS_DIMENSION && scene.height == 13.0);
    CHECK(scene.shape_count == 3);
    const mv_shape *rect = &scene.shapes[0];
    const mv_shape *circle = &scene.shapes[1];
    const mv_shape *line = &scene.shapes[2];
    CHECK(rect->kind == MV_RECT && rect->loc.line == 7);
    CHECK(rect->values[0] == -6.0 && rect->values[1] == 0.0 && signbit(rect->values[1]));
    CHECK(rect->values[2] == 0.0 && rect->values[3] == 3.0);
    CHECK(rect->color[0] == 0xab && rect->color[1] == 0x09 && rect->color[2] == 0xfe);
    CHECK(circle->kind == MV_CIRCLE && circle->loc.line == 8);
    CHECK(circle->values[0] == 0.0 && circle->values[1] == -3.0);
    CHECK(circle->values[2] == 0.0 && signbit(circle->values[2]));
    CHECK(memcmp(circle->color, rect->color, sizeof(rect->color)) == 0);
    CHECK(line->kind == MV_LINE && line->loc.line == 9);
    CHECK(line->values[0] == 1e300 && line->values[1] == -6.0);
    CHECK(line->values[2] == 1e-310 && line->values[2] > 0.0);
    CHECK(line->values[3] == -3.0 && line->values[4] == 1.5);
    CHECK(line->color[0] == 1 && line->color[1] == 2 && line->color[2] == 3);
    mv_scene_destroy(&scene);
    mv_scene_destroy(&scene);
    CHECK(scene.shapes == NULL && scene.shape_count == 0);
}

static void test_nonfinite_ast(void)
{
    mv_expr width = {.kind = MV_EXPR_NUMBER, .loc = {7, 1, 8}, .number = INFINITY};
    mv_expr height = {.kind = MV_EXPR_NUMBER, .loc = {10, 1, 11}, .number = 10.0};
    mv_ast ast = {.width = &width, .height = &height};
    mv_scene scene = {0};
    mv_diagnostic diag = {0};
    CHECK(mv_lower(&ast, &scene, &diag) == MV_STATUS_SEMANTIC_ERROR);
    expect_location(diag, width.loc);
    CHECK(scene.shapes == NULL && scene.shape_count == 0);
}

static void expect_invalid_ir(const mv_scene *scene, mv_location expected)
{
    mv_diagnostic diag = {0};
    CHECK(mv_verify(scene, &diag) == MV_STATUS_INVALID_ARGUMENT);
    expect_location(diag, expected);
}

static void test_verify(void)
{
    const mv_shape valid[] = {
        {.kind = MV_RECT, .loc = {10, 2, 1}, .values = {-DBL_MAX, DBL_MAX, -0.0, 0.0}},
        {.kind = MV_CIRCLE, .loc = {20, 3, 1}, .values = {0.0, -1.0, 0.0}},
        {.kind = MV_LINE, .loc = {30, 4, 1}, .values = {0.0, 0.0, 1.0, 1.0, 0.5}},
    };
    const mv_scene empty = {.width = 20.0, .height = 30.0, .loc = {7, 1, 8}};
    mv_shape shapes[sizeof(valid) / sizeof(valid[0])];
    memcpy(shapes, valid, sizeof(shapes));
    mv_scene scene = empty;
    scene.shapes = shapes;
    scene.shape_count = sizeof(shapes) / sizeof(shapes[0]);

    CHECK(mv_verify(&empty, NULL) == MV_STATUS_OK);
    CHECK(mv_verify(&scene, NULL) == MV_STATUS_OK);
    expect_invalid_ir(NULL, (mv_location){0});

    mv_scene malformed = empty;
    malformed.width = 0.0;
    expect_invalid_ir(&malformed, empty.loc);
    malformed = empty;
    malformed.width = MV_MAX_CANVAS_DIMENSION + 1.0;
    expect_invalid_ir(&malformed, empty.loc);
    malformed = empty;
    malformed.height = INFINITY;
    expect_invalid_ir(&malformed, empty.loc);
    malformed = empty;
    malformed.height = NAN;
    expect_invalid_ir(&malformed, empty.loc);
    malformed = empty;
    malformed.shape_count = 1;
    expect_invalid_ir(&malformed, empty.loc);
    malformed = scene;
    malformed.shape_count = SIZE_MAX;
    expect_invalid_ir(&malformed, empty.loc);

    shapes[0].kind = (mv_shape_kind)99;
    expect_invalid_ir(&scene, valid[0].loc);
    shapes[0] = valid[0];
    shapes[0].values[3] = -1.0;
    expect_invalid_ir(&scene, valid[0].loc);
    shapes[0] = valid[0];
    shapes[1].values[2] = -1.0;
    expect_invalid_ir(&scene, valid[1].loc);
    shapes[1] = valid[1];
    shapes[1].values[1] = NAN;
    expect_invalid_ir(&scene, valid[1].loc);
    shapes[1] = valid[1];
    shapes[2].values[1] = -INFINITY;
    expect_invalid_ir(&scene, valid[2].loc);
    shapes[2] = valid[2];
    shapes[2].values[4] = -0.0;
    expect_invalid_ir(&scene, valid[2].loc);
}

int main(void)
{
    test_semantic_failures();
    test_lower_success();
    test_nonfinite_ast();
    test_verify();
    return EXIT_SUCCESS;
}
