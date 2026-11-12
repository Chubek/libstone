#ifndef MOONSTONE_PROLIFIC_H
#define MOONSTONE_PROLIFIC_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(PROLIFIC_SHARED)
#if defined(PROLIFIC_BUILDING)
#define PROLIFIC_API __declspec(dllexport)
#else
#define PROLIFIC_API __declspec(dllimport)
#endif
#else
#define PROLIFIC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PROLIFIC_ABI_VERSION 1u
typedef struct prolific_session prolific_session;
typedef enum prolific_status {
    PROLIFIC_OK = 0,
    PROLIFIC_INVALID_INPUT = 1,
    PROLIFIC_EXHAUSTED = 2,
    PROLIFIC_FAILURE = 3
} prolific_status;
typedef enum prolific_domain {
    PROLIFIC_COMPILER = 0,
    PROLIFIC_VM = 1,
    PROLIFIC_RUNTIME = 2
} prolific_domain;
typedef enum prolific_kind {
    PROLIFIC_INSTANT = 2,
    PROLIFIC_COUNTER = 3,
    PROLIFIC_ERROR = 4,
    PROLIFIC_ALLOCATION = 5,
    PROLIFIC_FREE = 6
} prolific_kind;
typedef struct prolific_config {
    uint32_t abi_version;
    size_t max_events, max_bytes, max_text, max_threads, max_depth;
} prolific_config;
typedef struct prolific_location {
    const char *file;
    const char *ir_id;
    uint32_t line, column;
} prolific_location;

/* Null config uses defaults; non-null config requires all nonzero limits,
 * max_events <= 1,000,000 and max_text <= 1 MiB (v1 report limits).
 * All strings must be valid UTF-8, NUL-terminated and valid for the call.
 * Optional location strings may be NULL. No pointers are retained.
 * Calls block/allocate, serialize per session, and are not signal-safe.
 * Spans are LIFO on their creating thread; no migration or cancellation.
 * Destroy only after joining producers. No exception crosses this ABI.
 */
PROLIFIC_API prolific_status prolific_create(const prolific_config *config, prolific_session **out);
PROLIFIC_API void prolific_destroy(prolific_session *session);
PROLIFIC_API prolific_status prolific_begin(prolific_session *session, prolific_domain domain,
                                            const char *name, const prolific_location *location,
                                            uint64_t *span);
PROLIFIC_API prolific_status prolific_end(prolific_session *session, uint64_t span);
/* Account for an observation dropped inside a host adapter before admission. */
PROLIFIC_API prolific_status prolific_record_loss(prolific_session *session);
PROLIFIC_API prolific_status prolific_emit(prolific_session *session, prolific_domain domain,
                                           prolific_kind kind, const char *name, int64_t value,
                                           const char *detail, const prolific_location *location);
/* Returned JSON is allocated by Prolific; release with prolific_free_string.
 * Cache run IDs are 1..128 ASCII letters/digits/dot/underscore/hyphen.
 * save rejects duplicates unless replace != 0; persists an atomic snapshot.
 */
PROLIFIC_API prolific_status prolific_report(prolific_session *session, char **json);
PROLIFIC_API prolific_status prolific_save(prolific_session *session, const char *directory,
                                           const char *run_id, int replace);
PROLIFIC_API void prolific_free_string(char *text);
/* Calling-thread diagnostic, valid until the next status-returning call. */
PROLIFIC_API const char *prolific_last_error(void);

#ifdef __cplusplus
}
#endif
#endif
