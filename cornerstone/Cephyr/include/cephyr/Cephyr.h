#ifndef CEPHYR_CEPHYR_H
#define CEPHYR_CEPHYR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cephyr_result cephyr_result;
typedef struct cephyr_sdk cephyr_sdk;

typedef struct cephyr_request {
    size_t struct_size;         /* Set to sizeof(cephyr_request); trailing fields are ignored. */
    size_t argc;
    const char *const *argv;     /* argc arguments; no program name or sentinel. */
    const char *stdin_data;     /* Bytes used for an input operand of "-". */
    size_t stdin_size;          /* Zero for empty input; no terminator needed. */
    const char *resource_root;  /* Directory containing stdlib/ and triples/, or NULL. */
    const char *archon_root;    /* Directory containing archon/ and stdspec/, or NULL. */
    const cephyr_sdk *sdk;      /* Optional borrowed SDK; see CephyrSDK.h. */
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
 * Compile synchronously. Request storage remains owned by the caller and must
 * stay valid and unmodified throughout this call. On success or compilation
 * failure, *result owns a result; release it with cephyr_result_destroy. On
 * API misuse or internal failure, *result may be NULL. The output slot is
 * overwritten but any existing result in it is not destroyed. Results are
 * independent of requests and later compilations. A failure can have output.
 *
 * Use -o - to capture assembly or MIR; -E captures output by default. Calls
 * can overlap on different threads, but same-thread recursion is rejected.
 * Opt into QML through the same argv options and TOML profiles as the CLI.
 * See EMBEDDING.md for host CPython lifecycle and native backend requirements.
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
