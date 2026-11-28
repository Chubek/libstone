#ifndef MAHAK_C_API_H
#define MAHAK_C_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAHAK_C_API_VERSION 1

/* Version and feature probes. Returned strings are static. */
const char *mahak_version(void);
unsigned mahak_api_version(void);
const char *mahak_result_schema(void);

/* Opaque engine handle. Create with mahak_engine_create; destroy exactly once.
 * One engine may run one session at a time. All functions are not async-signal-
 * safe. Error strings returned through out_error must be freed with mahak_free. */
typedef struct mahak_engine mahak_engine;

mahak_engine *mahak_engine_create(void);
void mahak_engine_destroy(mahak_engine *engine);

/* Validates a JSON manifest. Returns 0 when valid, 2 for configuration errors.
 * On failure *out_error receives an allocated diagnostic string (may be NULL). */
int mahak_validate_manifest(const char *path, char **out_error);

/* Runs a manifest. On success *out_json receives the allocated session JSON
 * document and the return value is 0. Execution failures return 3, invalid
 * configuration 2, internal errors 1. */
int mahak_run_manifest(mahak_engine *engine, const char *path, char **out_json,
                       char **out_error);

/* Cancels the active session. Safe to call from another thread. */
void mahak_engine_cancel(mahak_engine *engine);

/* Frees a string allocated by Mahak. Passing NULL is a no-op. */
void mahak_free(char *value);

#ifdef __cplusplus
}
#endif

#endif /* MAHAK_C_API_H */
