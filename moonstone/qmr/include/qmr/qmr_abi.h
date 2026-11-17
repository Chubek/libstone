#ifndef QMR_ABI_H
#define QMR_ABI_H

/* Versioned C ABI over the stable C++ QMR API.
 *
 * Stability contract: qmr_abi_version() must equal the QMR_ABI_VERSION this
 * header was compiled against; symbol names and the meaning of existing
 * enum/struct fields never change within one version. New fields are appended
 * with explicit defaults; callers should zero-init structs and set the
 * desired limits.
 *
 * The ABI is deliberately small: engines render templates with flat
 * string key/value contexts. Richer models use the C++ API.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QMR_ABI_VERSION 1

/* Keep in sync with moonstone::qmr::Codec. */
enum {
    QMR_CODEC_HTML = 0,
    QMR_CODEC_PLAIN = 1,
    QMR_CODEC_JSON = 2,
    QMR_CODEC_XML = 3
};

/* Zero-initialized fields fall back to the C++ Limits defaults:
 * pass limits = NULL for all defaults. Durations are in milliseconds;
 * a negative or zero duration disables the wall-clock deadline.
 * A zero memory/instruction/include limit is invalid and rejected. */
typedef struct qmr_limits {
    size_t max_template_bytes;
    size_t max_compiled_bytes;
    size_t max_output_bytes;
    size_t max_context_bytes;
    size_t max_nodes;
    size_t max_include_depth;
    size_t max_instructions;
    size_t max_value_depth;
    size_t max_memory_bytes;
    long long max_duration_ms;
} qmr_limits;

typedef struct qmr_engine qmr_engine;

/* Version of the ABI implemented by the loaded library. */
int qmr_abi_version(void);
/* Human-readable QMR version string; valid for the library lifetime. */
const char *qmr_version_string(void);

/* Creates an engine handle or NULL on failure (bad codec/limits). */
qmr_engine *qmr_engine_create(const qmr_limits *limits, int codec);
void qmr_engine_destroy(qmr_engine *engine);

/* Renders a template from text. keys/values supply a flat string context
 * (value NULL maps to a nil model value). On success returns 0 and sets
 * *out to a NUL-terminated malloc'd buffer to release with qmr_free.
 * On failure returns nonzero and sets *error to a malloc'd message;
 * *out is left unmodified. template_name may be NULL. */
int qmr_engine_render(qmr_engine *engine, const char *template_text,
                      const char *template_name, const char *const *keys,
                      const char *const *values, size_t count,
                      char **out, char **error);

/* Renders a template file under an optional root directory (NULL root =
 * the file's parent directory). Same output/error contract. */
int qmr_engine_render_file(qmr_engine *engine, const char *path, const char *root,
                           const char *const *keys, const char *const *values,
                           size_t count, char **out, char **error);

/* Frees a buffer returned by the ABI. NULL is ignored. */
void qmr_free(char *buffer);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QMR_ABI_H */
