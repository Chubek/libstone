#ifndef BLACKSTONE_H
#define BLACKSTONE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct blackstone_runtime blackstone_runtime;
typedef struct lua_State lua_State;
const char *blackstone_version(void);

/* Push exactly one context table. Called inside a protected Lua call.
 * The callback must not throw C++ exceptions. The context belongs to the host. */
typedef int (*blackstone_push_context)(lua_State *, void *);

typedef struct blackstone_kernel_info {
    const char *name;
    const char *ir;
    const char *phase;
    const char *source;
    const char *documentation;
} blackstone_kernel_info;

blackstone_runtime *blackstone_create(void);
void blackstone_destroy(blackstone_runtime *runtime);
/* Zero is success, -1 is error. Errors are retained until the next operation. */
int blackstone_load_file(blackstone_runtime *, const char *path);
int blackstone_load_directory(blackstone_runtime *, const char *path);
size_t blackstone_kernel_count(const blackstone_runtime *);
/* Returned strings remain valid until the next successful load or destruction. */
int blackstone_kernel_get(const blackstone_runtime *, size_t index, blackstone_kernel_info *);
size_t blackstone_requirement_count(const blackstone_runtime *, size_t index);
const char *blackstone_requirement_get(const blackstone_runtime *, size_t index, size_t requirement);
size_t blackstone_preservation_count(const blackstone_runtime *, size_t index);
const char *blackstone_preservation_get(const blackstone_runtime *, size_t index, size_t preservation);
const char *blackstone_last_error(const blackstone_runtime *);
/* Required analyses must be named in available. changed receives a strict Lua
 * boolean result. Generic host contexts are responsible for their own rollback. */
int blackstone_run(blackstone_runtime *, const char *name, const char *ir,
                  const char *const *available, size_t available_count,
                  blackstone_push_context, void *user, int *changed);

#ifdef __cplusplus
}
#endif
#endif
