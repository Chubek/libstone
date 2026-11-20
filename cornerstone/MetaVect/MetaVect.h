#ifndef METAVECT_H
#define METAVECT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define METAVECT_VERSION_MAJOR 0
#define METAVECT_VERSION_MINOR 1
#define METAVECT_VERSION_PATCH 0

typedef enum mv_status {
    MV_STATUS_OK = 0,
    MV_STATUS_INVALID_ARGUMENT = 1,
    MV_STATUS_OUT_OF_MEMORY = 2,
    MV_STATUS_NOT_IMPLEMENTED = 3,
    MV_STATUS_SYNTAX_ERROR = 4,
    MV_STATUS_SEMANTIC_ERROR = 5,
    MV_STATUS_BACKEND_ERROR = 6,
    MV_STATUS_LIMIT_EXCEEDED = 7
} mv_status;

typedef enum mv_backend {
    MV_BACKEND_SVG = 0,
    MV_BACKEND_EPS = 1
} mv_backend;

typedef struct mv_output {
    char *data;
    size_t size; /* Excludes the terminating NUL. */
} mv_output;

typedef struct mv_diagnostic {
    size_t offset; /* Zero-based byte offset. */
    size_t line;   /* One-based source line. */
    size_t column; /* One-based byte column. */
    const char *message; /* Static storage; do not free. */
} mv_diagnostic;

const char *mv_version_string(void);
const char *mv_status_string(mv_status status);

/*
 * Compile exactly source_size bytes from non-NULL source. The bytes must be
 * readable for the duration of the call; no NUL terminator is needed. The
 * library does not retain source.
 *
 * output is required and is initialized by this function, even on failure.
 * Destroy a previous result before reusing its mv_output. On success, the
 * caller owns output->data; it is NUL-terminated. On failure, output is empty.
 *
 * diag is optional. A provided diagnostic is cleared on success. API argument
 * errors have zero offset, line, and column.
 */
mv_status mv_compile(const char *source, size_t source_size, mv_backend backend,
                     mv_output *output, mv_diagnostic *diag);

/* Release an initialized output and clear it. NULL and repeated calls are safe. */
void mv_output_destroy(mv_output *output);

#ifdef __cplusplus
}
#endif
#endif
