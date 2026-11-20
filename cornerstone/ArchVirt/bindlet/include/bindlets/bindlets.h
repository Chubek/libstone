#ifndef BINDLETS_H
#define BINDLETS_H
/**
 * bindlets.h — Stable C ABI for the Domfix bindlet engine
 * =======================================================
 *
 * All types are opaque and forward-compatible.  Every function returns
 * an ``int`` error code (0 = success, nonzero = ``c_bindlet_error_t``).
 * Output parameters are owned by the caller and must be freed with the
 * corresponding ``_free_*`` function.
 *
 * Thread safety:  the underlying C++ wrapper acquires the GIL
 * internally, so calls from different threads are safe as long as
 * they operate on *different* ``bindlet_t`` handles.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handle ───────────────────────────────────────────────── */

typedef struct bindlet_t bindlet_t;

/* ── Error codes (mirrors Python BindletErrorCode / C++ ErrorCode) ─ */

typedef enum {
    BINDLET_OK = 0,
    BINDLET_ERR_NOT_FOUND = 1,
    BINDLET_ERR_CONFIG_INVALID = 2,
    BINDLET_ERR_VERSION_MISMATCH = 3,
    BINDLET_ERR_EXECUTION_FAILED = 4,
    BINDLET_ERR_INPUT_MALFORMED = 5,
    BINDLET_ERR_TIMEOUT = 6,
    BINDLET_ERR_INTERNAL = 99,
} c_bindlet_error_t;

/* ── Severity & confidence ───────────────────────────────────────── */

typedef enum {
    BINDLET_SEVERITY_STYLE = 0,
    BINDLET_SEVERITY_INFORMATION = 1,
    BINDLET_SEVERITY_WARNING = 2,
    BINDLET_SEVERITY_ERROR = 3,
    BINDLET_SEVERITY_CRITICAL = 4,
} c_severity_t;

typedef enum {
    BINDLET_CONFIDENCE_LOW = 0,
    BINDLET_CONFIDENCE_MEDIUM = 1,
    BINDLET_CONFIDENCE_HIGH = 2,
} c_confidence_t;

/* ── Structured types ────────────────────────────────────────────── */

typedef struct {
    char* file;
    int   line_start;
    int   line_end;
    int   col_start;
    int   col_end;
} c_source_span_t;

typedef struct {
    char* description;
    char* replacement;
    c_source_span_t span;
    int   has_span;           /* 1 if span is meaningful */
} c_fix_suggestion_t;

typedef struct {
    char*           rule_id;
    char*           message;
    c_severity_t    severity;
    c_confidence_t  confidence;
    c_source_span_t location;
    int*            cwe_ids;
    int             cwe_count;
    c_fix_suggestion_t fix;
    int             has_fix;  /* 1 if fix is meaningful */
    char**          extra_keys;
    char**          extra_values;
    int             extra_count;
} c_analysis_issue_t;

typedef struct {
    char*  name;
    char*  description;
    int    input_kind;        /* 0=ast_json, 1=token_stream, 2=source_text, 3=cfg */
    char** output_kinds;
    int    output_kind_count;
    char** tags;
    int    tag_count;
} c_domainlet_capability_t;

typedef struct {
    char*  name;
    char*  version;
    int    api_version;
    char*  description;
    char*  author;
    c_domainlet_capability_t* capabilities;
    int    capability_count;
} c_domainlet_info_t;

/* ── Error detail ────────────────────────────────────────────────── */

typedef struct {
    c_bindlet_error_t code;
    char* message;
    char* source;
    char* detail;
} c_bindlet_error_detail_t;

/* ── Lifecycle ───────────────────────────────────────────────────── */

/**
 * Boot the embedded Python interpreter, add *domainlet_path* to
 * sys.path.  Safe to call multiple times.
 *
 * \param domainlet_path  filesystem path to the directory containing
 *                        the ``domainlet`` Python package.
 * \return 0 on success.
 */
int bindlets_init_runtime(const char* domainlet_path);

/** Shut down the runtime.  All outstanding ``bindlet_t`` handles
 *  must be destroyed first. */
void bindlets_shutdown_runtime(void);

/**
 * Create a bindlet handle by importing *module_name* and
 * instantiating *class_name*.
 *
 * \param[out] handle  receives the new opaque handle.
 * \return 0 on success, BINDLET_ERR_NOT_FOUND if unresolvable.
 */
int bindlets_create(bindlet_t** handle,
                    const char* module_name,
                    const char* class_name);

/** Destroy a bindlet handle, calling teardown() on the Python side. */
void bindlets_destroy(bindlet_t* handle);

/* ── Operations ──────────────────────────────────────────────────── */

/**
 * Probe: retrieve metadata and capabilities without heavy init.
 *
 * \param[out] info  caller must free with ``bindlets_free_info()``.
 */
int bindlets_probe(bindlet_t* handle, c_domainlet_info_t** info);

/**
 * Configure: apply string→string config.
 *
 * \param keys, values  parallel arrays of size *config_size*.
 * \param[out] err      caller must free with ``bindlets_free_error()``.
 */
int bindlets_configure(bindlet_t* handle,
                       const char** keys,
                       const char** values,
                       int config_size,
                       c_bindlet_error_detail_t** err);

/**
 * Execute: run analysis.
 *
 * \param ast_json       analysis payload (JSON, source text, etc.)
 * \param source_file    source file name for context
 * \param language       language identifier (e.g. "c", "cpp")
 * \param define_keys, define_values  preprocessor defines (parallel)
 * \param define_count
 * \param include_paths  include directories
 * \param include_count
 * \param option_keys, option_values  additional options (parallel)
 * \param option_count
 * \param[out] issues    caller must free with ``bindlets_free_issues()``.
 * \param[out] count     number of issues written.
 * \param[out] err       caller must free with ``bindlets_free_error()``.
 */
int bindlets_execute(bindlet_t* handle,
                     const char* ast_json,
                     const char* source_file,
                     const char* language,
                     const char** define_keys,
                     const char** define_values,
                     int define_count,
                     const char** include_paths,
                     int include_count,
                     const char** option_keys,
                     const char** option_values,
                     int option_count,
                     c_analysis_issue_t** issues,
                     int* count,
                     c_bindlet_error_detail_t** err);

/* ── Memory management ───────────────────────────────────────────── */

void bindlets_free_info(c_domainlet_info_t* info);
void bindlets_free_issues(c_analysis_issue_t* issues, int count);
void bindlets_free_error(c_bindlet_error_detail_t* err);

/* ── Utility ─────────────────────────────────────────────────────── */

/** Return a human-readable string for an error code. */
const char* bindlets_error_string(c_bindlet_error_t code);

/** Return the bindlet C ABI version (currently 1). */
int bindlets_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif /* BINDLETS_H */
