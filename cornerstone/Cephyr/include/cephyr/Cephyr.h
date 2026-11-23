#ifndef CEPHYR_CEPHYR_H
#define CEPHYR_CEPHYR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cephyr_result cephyr_result;
typedef struct cephyr_sdk cephyr_sdk;
typedef struct cephyr_context cephyr_context;
typedef struct cephyr_module cephyr_module;

#define CEPHYR_LUA_MAX_SCRIPTS 8u
#define CEPHYR_LUA_MAX_SCRIPT_BYTES (1024u * 1024u)
#define CEPHYR_LUA_MAX_NAME_BYTES 4096u
#define CEPHYR_LUA_MAX_ENV_ENTRIES 64u
#define CEPHYR_LUA_MAX_ENV_NAME_BYTES 128u
#define CEPHYR_LUA_MAX_ENV_VALUE_BYTES 4096u

typedef struct cephyr_lua_script {
    const char *source; /* Lua source bytes, not bytecode. NULL only for size zero. */
    size_t size;
    const char *name; /* Optional diagnostic label (not a path to load). */
} cephyr_lua_script;

typedef struct cephyr_lua_env_entry {
    const char *name; /* Unique ASCII [A-Za-z_][A-Za-z0-9_]* key. */
    const char *value; /* NUL-terminated; may be empty. */
} cephyr_lua_env_entry;

typedef struct cephyr_lua_options {
    size_t struct_size; /* Set to sizeof(cephyr_lua_options). */
    const cephyr_lua_script *scripts;
    size_t script_count;
    const cephyr_lua_env_entry *environment; /* Visible to every script in this request. */
    size_t environment_count;
} cephyr_lua_options;

typedef struct cephyr_request {
    size_t struct_size;         /* Set to sizeof(cephyr_request); unknown trailing fields are ignored. */
    size_t argc;
    const char *const *argv;     /* argc arguments; no program name or sentinel. */
    const char *stdin_data;     /* Bytes used for an input operand of "-". */
    size_t stdin_size;          /* Zero for empty input; no terminator needed. */
    const char *resource_root;  /* Directory containing stdlib/ and triples/, or NULL. */
    const char *archon_root;    /* Directory containing archon/ and stdspec/, or NULL. */
    const cephyr_sdk *sdk;      /* Optional borrowed SDK; see CephyrSDK.h. */
    const cephyr_lua_options *lua; /* Optional borrowed scripts and explicit environment. */
} cephyr_request;

typedef enum cephyr_status {
    CEPHYR_OK = 0,
    CEPHYR_COMPILE_ERROR = 1,
    CEPHYR_INVALID_ARGUMENT = 2,
    CEPHYR_INTERNAL_ERROR = 3,
    CEPHYR_LOAD_ERROR = 4,
    CEPHYR_SYMBOL_NOT_FOUND = 5
} cephyr_status;

unsigned cephyr_abi_version(void);
const char *cephyr_version(void);
/* Embedded CPython ABI if present (major << 16 | minor); zero without libpython. */
unsigned cephyr_python_version(void);

/*
 * Compile synchronously. Request storage remains owned by the caller and must
 * stay valid and unmodified throughout this call. On success or compilation
 * failure, *result owns a result; release it with cephyr_result_destroy. On
 * API misuse or internal failure, *result may be NULL. The output slot is
 * overwritten but any existing result in it is not destroyed. Results are
 * independent of requests and later compilations. A failure can have output.
 *
 * Use -o - to capture assembly, MIR, a relocatable object (-c), or a shared
 * object (-shared); -E captures preprocessed text by default. Executable
 * capture is not supported. Calls can overlap on different threads, but
 * same-thread recursion is rejected.
 * Opt into QML through the same argv options and TOML profiles as the CLI.
 * Pass Lua extensions through `lua`, or use the driver's files and configuration.
 * Do not combine `lua` with driver Lua options or configured Lua scripts.
 * See extension/README.md for the sandbox's capabilities and limits.
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

typedef struct cephyr_context_options {
    size_t struct_size;         /* Set to sizeof(cephyr_context_options); trailing fields are ignored. */
    size_t argc;
    const char *const *argv;     /* Fixed driver arguments, including "-" to compile source bytes. */
    const char *resource_root;  /* As in cephyr_request. */
    const char *archon_root;    /* As in cephyr_request. */
} cephyr_context_options;

/*
 * Create a context that owns an immutable copy of argv strings and roots.
 * *context is overwritten (NULL on failure), not destroyed. Driver options
 * and external configuration are evaluated on each compile. Source and SDK
 * storage are borrowed only during that call, and result handling is the
 * same as in cephyr_compile. This single-source call ignores the translation
 * units and host symbols registered with the additive API below.
 * A context can be shared by concurrent callers under the normal compile
 * rules; destruction must not overlap its use. Destroy accepts NULL.
 */
int cephyr_context_create(const cephyr_context_options *options, cephyr_context **context);
int cephyr_context_compile(const cephyr_context *context, const char *source, size_t source_size,
                           const cephyr_sdk *sdk, cephyr_result **result);
void cephyr_context_destroy(cephyr_context *context);

/*
 * Append a copy of one C translation unit (up to 16 MiB). name is a logical
 * source filename used for diagnostics, __FILE__ and relative quoted includes;
 * NULL selects a synthetic name. No compilation occurs here. Link requires a nonempty
 * collection and exactly one C "-" input operand (expanded in append order),
 * with -shared -o -. Link recompiles a snapshot, runs the optional borrowed SDK
 * for each unit, and does not consume collection state even on failure.
 * Mutations and snapshots are serialized; no lock is held while compiling.
 * reset clears both units and host registrations, not arguments or roots.
 * Existing results and loaded modules are not affected by reset or destruction.
 */
int cephyr_context_add_translation_unit(cephyr_context *context, const char *name,
                                        const char *source, size_t source_size);
int cephyr_context_link(const cephyr_context *context, const cephyr_sdk *sdk,
                        cephyr_result **result);
int cephyr_context_reset(cephyr_context *context);

typedef enum cephyr_host_symbol_kind {
    CEPHYR_HOST_FUNCTION = 1,
    CEPHYR_HOST_DATA = 2
} cephyr_host_symbol_kind;

/*
 * Register (or replace) a host import by C identifier for subsequent links.
 * Supported with the direct ld.lld linker on SysV AMD64 Linux for host-ABI modules.
 * A function address (POSIX function-pointer conversion) is called through
 * a module-local trampoline; a data address identifies the actual storage.
 * Registered names are reserved for imported symbols, not TU definitions;
 * the __wrap_ and __real_ prefixes are reserved by the linker. No process-wide
 * symbols are exported. Result bytes with bindings are process-specific.
 * The caller must keep addresses and backing code/storage valid as long as
 * any resulting artifact may be loaded or a module can still use them
 * (including in its destructors).
 * Reset and replacement never revoke addresses in old results or modules.
 */
int cephyr_context_register_host_symbol(cephyr_context *context, const char *name,
                                        cephyr_host_symbol_kind kind, const void *address);

/*
 * Explicitly load a successful -shared -o - result using the Linux dynamic
 * loader (RTLD_NOW | RTLD_LOCAL). TRUSTED host-ABI code only: loading can
 * execute constructors. The artifact uses a Linux memory file if available;
 * otherwise it is staged privately and unlinked after loading. The result may
 * be destroyed after this call. On failure *module is NULL; optional *details can own a plain-text
 * diagnostic result on CEPHYR_LOAD_ERROR (release with cephyr_result_destroy).
 * On success *details is NULL. Output slots are overwritten, not destroyed.
 */
int cephyr_module_load(const cephyr_result *artifact, cephyr_module **module,
                       cephyr_result **details);

/*
 * Resolve a symbol in the module's loader scope. *address is cleared on
 * failure. CEPHYR_SYMBOL_NOT_FOUND means lookup failed; a successful lookup
 * can legally produce a NULL symbol address. Addresses are borrowed: stop
 * using them and quiesce module code before cephyr_module_destroy, which
 * releases the loader reference and can execute destructors. Destroy accepts
 * NULL and must not overlap a lookup or execution by another thread.
 */
int cephyr_module_symbol(const cephyr_module *module, const char *name, void **address);
void cephyr_module_destroy(cephyr_module *module);

#ifdef __cplusplus
}
#endif

#endif
