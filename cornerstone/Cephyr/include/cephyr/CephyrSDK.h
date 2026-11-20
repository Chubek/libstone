#ifndef CEPHYR_SDK_H
#define CEPHYR_SDK_H

#include "Cephyr.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CEPHYR_SDK_ABI_VERSION 1u
#define CEPHYR_SDK_MIR_SCHEMA_VERSION 1u

#define CEPHYR_SDK_MAX_HOOKS 64u
#define CEPHYR_SDK_MAX_DIAGNOSTICS 256u
#define CEPHYR_SDK_MAX_SOURCE_BYTES (16u * 1024u * 1024u)
#define CEPHYR_SDK_MAX_MIR_BYTES (4u * 1024u * 1024u)
#define CEPHYR_SDK_MAX_ASSEMBLY_BYTES (16u * 1024u * 1024u)
#define CEPHYR_SDK_MAX_NAME_BYTES 96u
#define CEPHYR_SDK_MAX_TRIPLE_BYTES 128u
#define CEPHYR_SDK_MAX_MESSAGE_BYTES 4096u
#define CEPHYR_SDK_MAX_FILE_BYTES 4096u

/* A byte span, not a C string. Null is valid only for an empty span.
 * Embedded NUL bytes are not permitted in values submitted by SDK clients. */
typedef struct cephyr_sdk_text {
    const char *data;
    size_t size;
} cephyr_sdk_text;

enum {
    CEPHYR_SDK_DESUGAR = 1,
    CEPHYR_SDK_ANALYZE = 2,
    CEPHYR_SDK_REWRITE = 3,
    CEPHYR_SDK_ASSEMBLY = 4
};

enum {
    CEPHYR_SDK_NOTE = 1,
    CEPHYR_SDK_WARNING = 2,
    CEPHYR_SDK_ERROR = 3
};

enum {
    CEPHYR_SDK_OK = 0,
    CEPHYR_SDK_REJECT = 1
};

enum {
    CEPHYR_SDK_POSITION_INDEPENDENT = 1u
};

/* Borrowed and immutable, valid only during the callback. `text` is raw C at
 * DESUGAR and Archon MachineIR text at the other stages. The compiler supplies
 * canonical MIR; once replaced, later hooks see the submitted text as-is.
 * The MIR schema and flags are zero when inapplicable. An ASSEMBLY hook may
 * receive CEPHYR_SDK_POSITION_INDEPENDENT in flags. */
typedef struct cephyr_sdk_input {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t stage;
    cephyr_sdk_text text;
    cephyr_sdk_text input_name;
    cephyr_sdk_text target_triple;
    cephyr_sdk_text target_architecture;
    cephyr_sdk_text target_abi;
    uint32_t mir_schema_version;
    uint32_t flags;
} cephyr_sdk_input;

/* These sinks may be called synchronously on the callback's thread only,
 * using the supplied context. They copy their inputs before returning.
 * Return value: 1 accepted, 0 invalid or out of budget. A failed submission
 * fails compilation even if ignored. The sinks and context expire when the
 * callback returns.
 *
 * set_text may be used once by a DESUGAR, REWRITE, or ASSEMBLY callback,
 * never by ANALYZE. No submission leaves a transformation unchanged.
 * ASSEMBLY must submit nonempty module assembly ending in a newline; see
 * SDK.md for the linkage/data contract. REWRITE must submit nonempty MIR
 * if replacing.
 *
 * report uses an ASCII code ([A-Za-z0-9_.-]+) and a nonempty message.
 * Supply an empty file span for the input name. Coordinates are 1-based;
 * zero means unknown, and a column requires a line. Coordinates are
 * client-provided and are not remapped. An error diagnostic rejects the
 * compilation even if the callback returns CEPHYR_SDK_OK. */
typedef struct cephyr_sdk_output {
    size_t struct_size;
    uint32_t abi_version;
    void *context;
    int (*set_text)(void *context, cephyr_sdk_text text);
    int (*report)(void *context, uint32_t severity, cephyr_sdk_text code,
                  cephyr_sdk_text message, cephyr_sdk_text file,
                  uint32_t line, uint32_t column);
} cephyr_sdk_output;

/* Return CEPHYR_SDK_OK to commit, or CEPHYR_SDK_REJECT to fail the request.
 * This is trusted in-process code: it must not throw across the C API. */
typedef uint32_t (*cephyr_sdk_callback)(const cephyr_sdk_input *input,
                                        const cephyr_sdk_output *output,
                                        void *user);

/* Fixed-layout v1 descriptor. Names are unique ASCII identifiers (same
 * character set as diagnostic codes). An empty target triple matches all
 * targets except at ASSEMBLY, which requires an exact target triple and is
 * unique per triple. Matching is case-sensitive. */
typedef struct cephyr_sdk_hook {
    uint32_t stage;
    cephyr_sdk_text name;
    cephyr_sdk_text target_triple;
    cephyr_sdk_callback callback;
    void *user;
} cephyr_sdk_hook;

/* Explicit registration via `cephyr_request.sdk`. A null request field
 * disables all hooks. The descriptor, hook array, their strings, and user
 * state must remain valid for the duration of the compile call. Invocation
 * is synchronous and in array order within each stage. Do not reenter
 * cephyr_compile on the callback's thread. Different concurrent compile
 * calls may invoke the same hooks concurrently; synchronize mutable client
 * state accordingly. Cephyr does not load SDK code from disk. */
struct cephyr_sdk {
    size_t struct_size;
    uint32_t abi_version;
    const cephyr_sdk_hook *hooks;
    size_t hook_count;
};

#ifdef __cplusplus
}
#endif

#endif
