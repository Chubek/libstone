#include "internal.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>

static int valid_dimension(double dimension)
{
    return isfinite(dimension) && dimension > 0.0 && dimension <= MV_MAX_CANVAS_DIMENSION;
}

static mv_status verify_shape(const mv_shape *shape, mv_diagnostic *diag)
{
    size_t value_count;
    switch (shape->kind) {
    case MV_RECT: value_count = 4; break;
    case MV_CIRCLE: value_count = 3; break;
    case MV_LINE: value_count = 5; break;
    default:
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, shape->loc, "invalid MVIR shape kind");
    }
    /* Only the operands defined by this kind are part of its geometry. */
    for (size_t i = 0; i < value_count; ++i) {
        if (!isfinite(shape->values[i]))
            return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, shape->loc,
                           "MVIR shape values must be finite");
    }
    if ((shape->kind == MV_RECT && (shape->values[2] < 0.0 || shape->values[3] < 0.0)) ||
        (shape->kind == MV_CIRCLE && shape->values[2] < 0.0))
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, shape->loc,
                       "MVIR rectangle dimensions and circle radii must be nonnegative");
    if (shape->kind == MV_LINE && shape->values[4] <= 0.0)
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, shape->loc,
                       "MVIR line stroke width must be positive");
#if UCHAR_MAX > 255
    for (size_t i = 0; i < 3; ++i) {
        if (shape->color[i] > 255)
            return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, shape->loc,
                           "MVIR color channels must be in the range 0-255");
    }
#endif
    return MV_STATUS_OK;
}

mv_status mv_verify(const mv_scene *scene, mv_diagnostic *diag)
{
    if (scene == NULL)
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, (mv_location){0}, "missing MVIR scene");
    if (!valid_dimension(scene->width) || !valid_dimension(scene->height))
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, scene->loc, "invalid MVIR canvas dimensions");
    if (scene->shape_count > SIZE_MAX / sizeof(*scene->shapes) ||
        (scene->shape_count != 0 && scene->shapes == NULL))
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, scene->loc, "invalid MVIR shape storage");

    for (size_t i = 0; i < scene->shape_count; ++i) {
        mv_status status = verify_shape(&scene->shapes[i], diag);
        if (status != MV_STATUS_OK)
            return status;
    }
    return MV_STATUS_OK;
}
