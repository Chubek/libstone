#ifndef METAVECT_INTERNAL_H
#define METAVECT_INTERNAL_H

#include "MetaVect.h"

typedef struct {
    const char *data;
    size_t size;
} mv_slice;

typedef struct {
    size_t offset, line, column;
} mv_location;

typedef enum {
    MV_EXPR_NUMBER, MV_EXPR_STRING, MV_EXPR_NAME, MV_EXPR_CALL
} mv_expr_kind;

typedef struct mv_expr {
    mv_expr_kind kind;
    mv_location loc;
    double number;
    mv_slice text;
    struct mv_expr **args;
    size_t arg_count;
} mv_expr;

typedef enum { MV_STMT_LET, MV_STMT_CALL } mv_stmt_kind;

typedef struct {
    mv_stmt_kind kind;
    mv_location loc;
    mv_slice name;
    mv_expr **args;
    size_t arg_count;
} mv_stmt;

/* Slices borrow the source. The AST owns the nodes and argument arrays. */
typedef struct {
    mv_expr *width, *height;
    mv_stmt *stmts;
    size_t stmt_count;
} mv_ast;

typedef enum { MV_RECT, MV_CIRCLE, MV_LINE } mv_shape_kind;

typedef struct {
    mv_shape_kind kind;
    mv_location loc;
    /* Rect: x/y/width/height; circle: cx/cy/radius;
       line: x1/y1/x2/y2/stroke width. */
    double values[5];
    unsigned char color[3];
} mv_shape;

#define MV_MAX_CANVAS_DIMENSION 2147483647.0

/* The scene owns a flat draw list and retains no source slices. */
typedef struct {
    double width, height;
    mv_location loc;
    mv_shape *shapes;
    size_t shape_count;
} mv_scene;

mv_status mv_fail(mv_diagnostic *diag, mv_status status, mv_location loc, const char *message);
int mv_slice_eq(mv_slice slice, const char *literal);

/* Input must already have been recognized as an ASCII decimal token. */
mv_status mv_parse_number(mv_slice token, double *result);
/* Locale-independent round-trip decimal formatting (snprintf convention). */
int mv_format_number(char *buffer, size_t capacity, double number);

mv_status mv_parse(const char *source, size_t size, mv_ast *ast, mv_diagnostic *diag);
void mv_ast_destroy(mv_ast *ast);
mv_status mv_lower(const mv_ast *ast, mv_scene *scene, mv_diagnostic *diag);
void mv_scene_destroy(mv_scene *scene);
mv_status mv_verify(const mv_scene *scene, mv_diagnostic *diag);
void mv_rewrite(mv_scene *scene);
mv_status mv_emit(const mv_scene *scene, mv_backend backend, mv_output *output,
                  mv_diagnostic *diag);

#endif
