#ifndef AUROCKS_FAULT_H
#define AUROCKS_FAULT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aurocks_fault_backend {
    AUROCKS_FAULT_EARLEY, AUROCKS_FAULT_LIBGLR
} aurocks_fault_backend;

typedef enum aurocks_fault_reason {
    AUROCKS_FAULT_SYNTAX, AUROCKS_FAULT_AMBIGUITY
} aurocks_fault_reason;

typedef enum aurocks_fault_disposition {
    AUROCKS_SILENT_PATCH, AUROCKS_LOUD_PATCH, AUROCKS_INFORM, AUROCKS_WARN, AUROCKS_ERROR
} aurocks_fault_disposition;

/* For LibGLR these pointers are glr_stack_t snapshots at the last visited
 * token boundary. Earley exposes its full frontier through the chart. */
typedef struct aurocks_fault_frontier {
    const void *const *stacks;
    size_t count, offset;
    int truncated;
} aurocks_fault_frontier;

/* Borrowed, read-only, valid only during a handler call. Native parser state
 * is the generated aurocks_parser for Earley or glr_parser_t for LibGLR. */
typedef struct aurocks_fault_state {
    aurocks_fault_backend backend;
    aurocks_fault_reason reason;
    const void *native_parser;
    const char *input;
    size_t length, offset, match_end;
    const char *original_input;
    size_t original_offset, retry;
    aurocks_fault_frontier frontier;
} aurocks_fault_state;

/* Defaults come from the matched rule. A handler may override these fields.
 * Offsets are half-open in the current input. Text must remain valid until
 * the attempt returns; do not supply addresses of handler-local variables. */
typedef struct aurocks_fault_response {
    size_t begin, end;
    const char *replacement;
    const char *message;
} aurocks_fault_response;

typedef void (*aurocks_fault_handler)(const aurocks_fault_state *, aurocks_fault_response *);

typedef struct aurocks_fault_rule {
    const char *pattern;
    aurocks_fault_disposition disposition;
    const char *replacement;
    const char *message;
    aurocks_fault_handler handler;
} aurocks_fault_rule;

/* Adapter API for generated parsers. Notify only for failures detected
 * before evaluation of semantic actions, while the native parser is live. */
typedef void (*aurocks_fault_observer)(void *, aurocks_fault_backend, aurocks_fault_reason,
                                      const void *, size_t, const aurocks_fault_frontier *);
typedef void *(*aurocks_fault_attempt)(const char *, const char **, aurocks_fault_observer,
                                      void *, const void *);
void *aurocks_fault_run(const char *input, const char **error, aurocks_fault_attempt attempt,
                       const void *parser, const aurocks_fault_rule *rules, size_t count);

#ifdef __cplusplus
}
#endif

#endif
