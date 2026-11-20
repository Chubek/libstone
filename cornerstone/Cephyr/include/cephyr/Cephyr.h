#ifndef CEPHYR_CEPHYR_H
#define CEPHYR_CEPHYR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cephyr_result cephyr_result;
typedef struct cephyr_sdk cephyr_sdk;

typedef struct cephyr_request {
    size_t struct_size;          /* Set to sizeof(cephyr_request). */
    size_t argc;
    const char *const *argv;     /* argc arguments; no program name or sentinel. */
    const char *stdin_data;     /* Bytes used for an input operand of "-". */
    size_t stdin_size;          /* Zero for empty input; no terminator needed. */
    const char *resource_root;  /* Directory containing stdlib/ and triples/, or NULL. */
    const char *archon_root;    /* Directory containing archon/ and stdspec/, or NULL. */
    const cephyr_sdk *sdk;      /* Optional borrowed SDK. */
} cephyr_request;

typedef enum cephyr_status {
    CEPHYR_OK = 0,
    CEPHYR_COMPILE_ERROR = 1,
    CEPHYR_INVALID_ARGUMENT = 2,
    CEPHYR_INTERNAL_ERROR = 3
} cephyr_status;

unsigned cephyr_abi_version(void);
const char *cephyr_version(void);
/* CPython build version, packed as (major << 16) | minor. */
unsigned cephyr_python_version(void);

/*
 * Compile synchronously. Request storage remains owned by the caller and need
 * only remain valid until this call returns. On success or compilation failure,
 * *result owns a result; release it with cephyr_result_destroy. On API misuse or
 * internal failure, *result may be NULL. An existing *result is not destroyed.
 * Use -o - to capture assembly or MIR; -E captures output by default.
 */
int cephyr_compile(const cephyr_request *request, cephyr_result **result);

/*
 * Return a NUL-terminated buffer valid until result destruction. If size is
 * non-NULL, receive the byte count excluding the terminator. A NULL result
 * returns NULL and sets the size to zero.
 */
const char *cephyr_result_output(const cephyr_result *result, size_t *size);
const char *cephyr_result_diagnostics(const cephyr_result *result, size_t *size);
void cephyr_result_destroy(cephyr_result *result);

#ifdef __cplusplus
}
#endif

#endif
