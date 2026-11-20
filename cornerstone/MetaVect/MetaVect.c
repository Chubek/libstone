#include "internal.h"

#include <stdlib.h>

const char *mv_version_string(void) { return "0.1.0"; }

const char *mv_status_string(mv_status status) {
    switch (status) {
    case MV_STATUS_OK: return "ok";
    case MV_STATUS_INVALID_ARGUMENT: return "invalid argument";
    case MV_STATUS_OUT_OF_MEMORY: return "out of memory";
    case MV_STATUS_NOT_IMPLEMENTED: return "not implemented";
    case MV_STATUS_SYNTAX_ERROR: return "syntax error";
    case MV_STATUS_SEMANTIC_ERROR: return "semantic error";
    case MV_STATUS_BACKEND_ERROR: return "backend error";
    case MV_STATUS_LIMIT_EXCEEDED: return "limit exceeded";
    default: return "unknown status";
    }
}

mv_status mv_compile(const char *source, size_t source_size, mv_backend backend,
                     mv_output *output, mv_diagnostic *diag) {
    const mv_location no_location = {0};
    mv_ast ast = {0};
    mv_scene scene = {0};
    mv_output result = {0};
    mv_status status;

    if (diag)
        *diag = (mv_diagnostic){0};
    if (!output)
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, no_location,
                       "output is required");
    *output = (mv_output){0};
    if (!source)
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, no_location,
                       "source is required");
    if (backend != MV_BACKEND_SVG && backend != MV_BACKEND_EPS)
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, no_location,
                       "invalid backend");

    status = mv_parse(source, source_size, &ast, diag);
    if (status != MV_STATUS_OK)
        goto done;
    status = mv_lower(&ast, &scene, diag);
    if (status != MV_STATUS_OK)
        goto done;
    status = mv_verify(&scene, diag);
    if (status != MV_STATUS_OK)
        goto done;
    mv_rewrite(&scene);
    status = mv_verify(&scene, diag);
    if (status != MV_STATUS_OK)
        goto done;
    status = mv_emit(&scene, backend, &result, diag);
    if (status == MV_STATUS_OK) {
        *output = result;
        result = (mv_output){0};
        if (diag)
            *diag = (mv_diagnostic){0};
    }

done:
    mv_output_destroy(&result);
    mv_scene_destroy(&scene);
    mv_ast_destroy(&ast);
    return status;
}

void mv_output_destroy(mv_output *output) {
    if (output) {
        free(output->data);
        *output = (mv_output){0};
    }
}
