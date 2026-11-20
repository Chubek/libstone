#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum { VALUE_NUMBER, VALUE_COLOR } value_kind;

typedef struct {
    value_kind kind;
    double number;
    unsigned char color[3];
} value;

typedef struct {
    mv_slice name;
    int initialized;
    value contents;
} binding;

typedef struct {
    binding *entries;
    size_t count;
    mv_diagnostic *diag;
} environment;

static int compare_names(mv_slice a, mv_slice b)
{
    size_t common = a.size < b.size ? a.size : b.size;
    int order = common == 0 ? 0 : memcmp(a.data, b.data, common);
    if (order != 0)
        return order;
    return (a.size > b.size) - (a.size < b.size);
}

static int compare_bindings(const void *a, const void *b)
{
    return compare_names(((const binding *)a)->name, ((const binding *)b)->name);
}

/* Use the first slot for a name even when the source declares it more than once. */
static binding *find_binding(const environment *env, mv_slice name)
{
    size_t first = 0;
    size_t end = env->count;
    while (first < end) {
        size_t mid = first + (end - first) / 2;
        if (compare_names(env->entries[mid].name, name) < 0)
            first = mid + 1;
        else
            end = mid;
    }
    if (first == env->count || compare_names(env->entries[first].name, name) != 0)
        return NULL;
    return &env->entries[first];
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_color(mv_slice text, unsigned char color[3])
{
    if (text.size != 7 || text.data[0] != '#')
        return 0;
    for (size_t i = 0; i < 3; ++i) {
        int high = hex_digit(text.data[1 + 2 * i]);
        int low = hex_digit(text.data[2 + 2 * i]);
        if (high < 0 || low < 0)
            return 0;
        color[i] = (unsigned char)(high * 16 + low);
    }
    return 1;
}

static mv_status eval(const environment *env, const mv_expr *expr, value *result);

static mv_status eval_number(const environment *env, const mv_expr *expr, double *number)
{
    value result;
    mv_status status = eval(env, expr, &result);
    if (status != MV_STATUS_OK)
        return status;
    if (result.kind != VALUE_NUMBER)
        return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, expr->loc, "expected a number");
    *number = result.number;
    return MV_STATUS_OK;
}

static mv_status eval_arithmetic(const environment *env, const mv_expr *expr, value *result)
{
    enum { ADD, SUB, MUL, DIV } op;
    if (mv_slice_eq(expr->text, "add"))
        op = ADD;
    else if (mv_slice_eq(expr->text, "sub"))
        op = SUB;
    else if (mv_slice_eq(expr->text, "mul"))
        op = MUL;
    else if (mv_slice_eq(expr->text, "div"))
        op = DIV;
    else
        return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, expr->loc,
                       "unknown arithmetic function");

    if (expr->arg_count != 2)
        return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, expr->loc,
                       "arithmetic functions take exactly two arguments");
    double a = 0.0, b = 0.0;
    mv_status status = eval_number(env, expr->args[0], &a);
    if (status != MV_STATUS_OK)
        return status;
    status = eval_number(env, expr->args[1], &b);
    if (status != MV_STATUS_OK)
        return status;

    double number = 0.0;
    switch (op) {
    case ADD: number = a + b; break;
    case SUB: number = a - b; break;
    case MUL: number = a * b; break;
    case DIV:
        if (b == 0.0)
            return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, expr->args[1]->loc,
                           "division by zero");
        number = a / b;
        break;
    }
    if (!isfinite(number))
        return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, expr->loc,
                       "arithmetic result must be finite");
    *result = (value){.kind = VALUE_NUMBER, .number = number};
    return MV_STATUS_OK;
}

static mv_status eval(const environment *env, const mv_expr *expr, value *result)
{
    switch (expr->kind) {
    case MV_EXPR_NUMBER:
        if (!isfinite(expr->number))
            return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, expr->loc,
                           "number must be finite");
        *result = (value){.kind = VALUE_NUMBER, .number = expr->number};
        return MV_STATUS_OK;
    case MV_EXPR_STRING:
        *result = (value){.kind = VALUE_COLOR};
        if (!parse_color(expr->text, result->color))
            return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, expr->loc,
                           "color must be # followed by six hexadecimal digits");
        return MV_STATUS_OK;
    case MV_EXPR_NAME: {
        const binding *entry = find_binding(env, expr->text);
        if (entry == NULL || !entry->initialized)
            return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, expr->loc,
                           "unknown or not yet defined binding");
        *result = entry->contents;
        return MV_STATUS_OK;
    }
    case MV_EXPR_CALL:
        return eval_arithmetic(env, expr, result);
    }
    return mv_fail(env->diag, MV_STATUS_INVALID_ARGUMENT, expr->loc, "invalid expression kind");
}

static mv_status lower_let(const environment *env, const mv_stmt *stmt)
{
    binding *entry = find_binding(env, stmt->name);
    if (entry == NULL || stmt->arg_count != 1 || stmt->args == NULL || stmt->args[0] == NULL)
        return mv_fail(env->diag, MV_STATUS_INVALID_ARGUMENT, stmt->loc, "invalid binding node");
    if (entry->initialized)
        return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, stmt->loc, "duplicate binding");
    mv_status status = eval(env, stmt->args[0], &entry->contents);
    if (status == MV_STATUS_OK)
        entry->initialized = 1;
    return status;
}

static mv_status lower_shape(const environment *env, const mv_stmt *stmt, mv_shape *shape)
{
    size_t expected_args, color_index;
    *shape = (mv_shape){.loc = stmt->loc};
    if (mv_slice_eq(stmt->name, "rect")) {
        shape->kind = MV_RECT;
        expected_args = 5;
        color_index = 4;
    } else if (mv_slice_eq(stmt->name, "circle")) {
        shape->kind = MV_CIRCLE;
        expected_args = 4;
        color_index = 3;
    } else if (mv_slice_eq(stmt->name, "line")) {
        shape->kind = MV_LINE;
        expected_args = 6;
        color_index = 4;
    } else {
        return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, stmt->loc, "unknown geometry command");
    }
    if (stmt->arg_count != expected_args)
        return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, stmt->loc,
                       "wrong number of geometry arguments");

    for (size_t i = 0; i < expected_args; ++i) {
        const mv_expr *arg = stmt->args[i];
        if (i == color_index) {
            value result;
            mv_status status = eval(env, arg, &result);
            if (status != MV_STATUS_OK)
                return status;
            if (result.kind != VALUE_COLOR)
                return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, arg->loc, "expected a color");
            memcpy(shape->color, result.color, sizeof(shape->color));
            continue;
        }
        double number = 0.0;
        mv_status status = eval_number(env, arg, &number);
        if (status != MV_STATUS_OK)
            return status;
        if (shape->kind == MV_RECT && (i == 2 || i == 3) && number < 0.0)
            return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, arg->loc,
                           "rectangle dimensions must be nonnegative");
        if (shape->kind == MV_CIRCLE && i == 2 && number < 0.0)
            return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, arg->loc,
                           "circle radius must be nonnegative");
        if (shape->kind == MV_LINE && i == 5 && number <= 0.0)
            return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, arg->loc,
                           "line stroke width must be positive");
        shape->values[i - (i > color_index)] = number;
    }
    return MV_STATUS_OK;
}

static mv_status lower_dimension(const environment *env, const mv_expr *expr, double *dimension)
{
    mv_status status = eval_number(env, expr, dimension);
    if (status != MV_STATUS_OK)
        return status;
    /* The ceiling of each dimension must fit a portable EPS BoundingBox. */
    if (*dimension <= 0.0 || *dimension > MV_MAX_CANVAS_DIMENSION)
        return mv_fail(env->diag, MV_STATUS_SEMANTIC_ERROR, expr->loc,
                       "canvas dimension must be positive and at most 2147483647");
    return MV_STATUS_OK;
}

mv_status mv_lower(const mv_ast *ast, mv_scene *scene, mv_diagnostic *diag)
{
    if (scene == NULL)
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, (mv_location){0}, "missing output scene");
    *scene = (mv_scene){0};
    if (ast == NULL || ast->width == NULL || ast->height == NULL)
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, (mv_location){0}, "missing canvas");

    mv_scene output = {.loc = ast->width->loc};
    if (ast->stmt_count != 0 && ast->stmts == NULL)
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, output.loc, "missing canvas statements");
    environment env = {.diag = diag};
    mv_status status = lower_dimension(&env, ast->width, &output.width);
    if (status != MV_STATUS_OK)
        return status;
    status = lower_dimension(&env, ast->height, &output.height);
    if (status != MV_STATUS_OK)
        return status;

    size_t shape_count = 0;
    for (size_t i = 0; i < ast->stmt_count; ++i) {
        if (ast->stmts[i].kind == MV_STMT_LET)
            ++env.count;
        else if (ast->stmts[i].kind == MV_STMT_CALL)
            ++shape_count;
    }
    if (env.count > SIZE_MAX / sizeof(*env.entries) || shape_count > SIZE_MAX / sizeof(*output.shapes))
        return mv_fail(diag, MV_STATUS_LIMIT_EXCEEDED, output.loc, "canvas has too many statements");

    if (env.count != 0) {
        env.entries = malloc(env.count * sizeof(*env.entries));
        if (env.entries == NULL)
            return mv_fail(diag, MV_STATUS_OUT_OF_MEMORY, output.loc, "cannot allocate bindings");
        /* Collect names for lookup without making their values visible in advance. */
        size_t next = 0;
        for (size_t i = 0; i < ast->stmt_count; ++i) {
            if (ast->stmts[i].kind == MV_STMT_LET)
                env.entries[next++] = (binding){.name = ast->stmts[i].name};
        }
        qsort(env.entries, env.count, sizeof(*env.entries), compare_bindings);
    }
    if (shape_count != 0) {
        output.shapes = malloc(shape_count * sizeof(*output.shapes));
        if (output.shapes == NULL) {
            status = mv_fail(diag, MV_STATUS_OUT_OF_MEMORY, output.loc, "cannot allocate shapes");
            goto done;
        }
    }

    for (size_t i = 0; i < ast->stmt_count; ++i) {
        const mv_stmt *stmt = &ast->stmts[i];
        switch (stmt->kind) {
        case MV_STMT_LET:
            status = lower_let(&env, stmt);
            break;
        case MV_STMT_CALL:
            status = lower_shape(&env, stmt, &output.shapes[output.shape_count]);
            if (status == MV_STATUS_OK)
                ++output.shape_count;
            break;
        default:
            status = mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, stmt->loc, "invalid statement kind");
            break;
        }
        if (status != MV_STATUS_OK)
            goto done;
    }

done:
    free(env.entries);
    if (status == MV_STATUS_OK)
        *scene = output;
    else
        mv_scene_destroy(&output);
    return status;
}

void mv_scene_destroy(mv_scene *scene)
{
    if (scene != NULL) {
        free(scene->shapes);
        *scene = (mv_scene){0};
    }
}
