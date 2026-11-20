#include "../internal.h"

void mv_rewrite(mv_scene *scene)
{
    size_t read, write = 0;

    /*
     * Empty fill elimination: in verified MVIR, a rectangle with either
     * dimension zero or a circle with radius zero paints nothing. Replace
     * that draw with no draw; this never grows the list. Stable compaction
     * copies surviving shapes unchanged, including their IEEE data.
     */
    for (read = 0; read < scene->shape_count; ++read) {
        const mv_shape *shape = &scene->shapes[read];

        if (shape->kind == MV_RECT &&
            (shape->values[2] == 0 || shape->values[3] == 0))
            continue;
        if (shape->kind == MV_CIRCLE && shape->values[2] == 0)
            continue;
        /* Stroked lines are not covered by the empty-fill rule. */
        if (write != read)
            scene->shapes[write] = *shape;
        ++write;
    }
    scene->shape_count = write;
}
