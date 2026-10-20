#ifndef FLINTSTONE_H
#define FLINTSTONE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct flintstone_context flintstone_context;
typedef struct flintstone_module flintstone_module;
typedef enum flintstone_status {
    FLINTSTONE_OK = 0, FLINTSTONE_INVALID_ARGUMENT = 1,
    FLINTSTONE_ERROR = 2, FLINTSTONE_OUT_OF_MEMORY = 3
} flintstone_status;
/* An integer code, not a C++ enum object: arbitrary foreign input can be
   validated without triggering undefined behavior on an out-of-range enum. */
typedef int flintstone_level;
enum {
    FLINTSTONE_HIGH = 0, FLINTSTONE_CORE = 1, FLINTSTONE_MACHINE = 2
};

const char* flintstone_version(void);
flintstone_context* flintstone_context_create(void);
void flintstone_context_destroy(flintstone_context*);
/* Thread-local error text; borrowed until the next API call on this thread. */
const char* flintstone_last_error(void);
/* Input is length-delimited; *out is NULL on failure. Modules own their data
   and may outlive the parsing context. Independent handles are independent. */
flintstone_status flintstone_parse(flintstone_context*, const char* source,
                                 size_t size, const char* filename, flintstone_module** out);
/* NUL-terminated convenience entry, NULL on failure; caller owns the result. */
flintstone_module* flintstone_parse_text(flintstone_context*, const char* source);
void flintstone_module_destroy(flintstone_module*);
flintstone_status flintstone_verify(flintstone_context*, const flintstone_module*);
/* Resolve `import` declarations from stdlib_dir (NULL uses default_stdlib_dir). */
flintstone_status flintstone_resolve_imports(flintstone_context*, flintstone_module*, const char* stdlib_dir);
flintstone_status flintstone_lower(flintstone_context*, flintstone_module*, flintstone_level);
flintstone_status flintstone_optimize(flintstone_context*, flintstone_module*,
                                    const char* kernel_directory, const char* kernel_name);
/* Text is owned by the module, valid until its next successful mutation/print. */
flintstone_status flintstone_print(flintstone_module*, const char** text);
const char* flintstone_module_text(flintstone_module*);
/* Integer-only convenience entry; full typed execution is in the C++ API.
   An exhausted instruction budget is an error, never a partial result. */
flintstone_status flintstone_run_i64(flintstone_context*, const flintstone_module*,
                                   const char* function, const int64_t* arguments,
                                   size_t argument_count, size_t budget, int64_t* result);
size_t flintstone_binding_target_count(void);
const char* flintstone_binding_target_at(size_t index);

#ifdef __cplusplus
}
#endif
#endif
