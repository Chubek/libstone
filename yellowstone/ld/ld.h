// ld.h - C API for the qobjld linker.
//
// This header is the C-accessible entry point to the linker facilities.
// The implementation lives in ld.c (compiled as C++ for access to the
// header-only readers in ../fd/). The C++ wrapper in ld.hpp builds on
// these functions.
#ifndef QOBJLD_LD_H
#define QOBJLD_LD_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(QLD_BUILD_SHARED)
#  ifdef qld_EXPORTS
#    define QLD_API __declspec(dllexport)
#  else
#    define QLD_API __declspec(dllimport)
#  endif
#else
#  define QLD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Link output mode. */
typedef enum qld_mode {
  QLD_MODE_EXEC = 0,      /* executable (default) */
  QLD_MODE_SHARED = 1,    /* ELF DSO / PE DLL / Mach-O dylib */
  QLD_MODE_RELOCATABLE = 2 /* partial link (-r), if supported by the backend */
} qld_mode_t;

/* Stable result codes returned by every operation. */
typedef enum qld_status {
  QLD_OK = 0,
  QLD_ERROR = 1,
  QLD_INVALID_ARGUMENT = 2,
  QLD_OUT_OF_MEMORY = 3,
  QLD_IO_ERROR = 4,
  QLD_PARSE_ERROR = 5,
  QLD_UNDEFINED_SYMBOL = 6,
  QLD_UNSUPPORTED = 7
} qld_status_t;

/* One-shot link options. All pointers are borrowed for the call duration;
 * none are retained. Strings use NUL-terminated UTF-8 path/byte semantics
 * of the host filesystem. */
typedef struct qld_options {
  const char* const* inputs;     /* object / archive / script paths */
  size_t n_inputs;
  const char* const* libpaths;   /* -L directories */
  size_t n_libpaths;
  const char* const* libs;       /* -l names (without lib prefix/suffix) */
  size_t n_libs;
  const char* output;            /* -o path, or NULL for in-memory only */
  const char* entry;             /* -e symbol, or NULL for default */
  const char* script;            /* -T path, or NULL */
  const char* script_text;       /* inline script, or NULL */
  int use_script_text;           /* nonzero when script_text is valid */
  uint64_t base;                 /* --image-base value */
  int has_base;                  /* nonzero when base is valid */
  qld_mode_t mode;               /* output mode */
  int pie;                       /* PIE executable; PE maps to ASLR */
  int gc_sections;               /* --gc-sections */
  int strip_debug;               /* -s: drop debug sections/symbols */
  int allow_undefined;           /* --allow-undefined */
  int verbose;                   /* --verbose */
  const char* map_file;          /* -Map path, or NULL */
  const char* emulator;          /* ELF emulation / PE machine / Mach-O arch */
} qld_options_t;

/* Initialize an options record to documented defaults. This is ABI-safe for
 * callers that do not use C designated initializers. */
QLD_API void qld_options_init(qld_options_t* opts);

/* Link once. Returns 0 on success, nonzero on failure.
 * On failure and when errmsg is non-NULL, *errmsg receives a malloc'd
 * NUL-terminated message the caller must release with qld_free_string(). */
QLD_API int qld_link(const qld_options_t* opts, char** errmsg);

/* Link to memory. Like qld_link but always captures the output image into
 * malloc'd bytes (*out_bytes, *out_size) regardless of opts->output.
 * The caller releases the buffer with qld_free_bytes(). When opts->output
 * is set the file is also written. */
QLD_API int qld_link_bytes(const qld_options_t* opts, uint8_t** out_bytes,
                    size_t* out_size, char** errmsg);

/* ELF backend v1. qld_options_t keeps its original ABI. A NULL linker uses
 * Yellowstone's native static/partial linker, including -L/-l searches.
 * Shared/PIE and dynamic imports are currently unsupported natively.
 * A non-NULL linker explicitly selects the optional external compatibility
 * bridge; merely selecting a format never invokes an external executable.
 * Arguments are individual argv tokens, appended after the structured options
 * in order (including position-dependent -l/--as-needed/--start-group flags).
 * Response files use GNU quoting. -o/--output overrides opts->output; absent
 * either, the result stays in memory. -T scripts use the ScriptSDK, including
 * Lua. All pointers are borrowed during a call, copied by qld_set_elf_options.
 * External compatibility bridges are disabled by default at build time. */
#define QLD_ELF_OPTIONS_VERSION 1
typedef struct qld_elf_options {
  size_t struct_size;                  /* sizeof(qld_elf_options_t) */
  const char *linker;                  /* NULL = native; executable = explicit external bridge */
  const char *const *arguments;
  size_t n_arguments;
  void (*diagnostic)(void *user, const char *text); /* successful-link warnings */
  void *user;
} qld_elf_options_t;
QLD_API void qld_elf_options_init(qld_elf_options_t *opts);
/* Configured external executable, or NULL when that compatibility bridge is disabled.
 * Native linking remains available in either case. */
QLD_API const char *qld_elf_linker(void);
QLD_API int qld_link_elf(const qld_options_t *, const qld_elf_options_t *, char **errmsg);
QLD_API int qld_link_elf_bytes(const qld_options_t *, const qld_elf_options_t *,
    uint8_t **out_bytes, size_t *out_size, char **errmsg);

/* PE/COFF and Mach-O backends v1.
 * The original qld_options_t ABI is unchanged; qld_link[_bytes] auto-detects
 * these formats from object/archive contents. Explicit entry points also work
 * for library-only links and ordered native arguments.
 *
 * NULL linker selects native fixed-base static images (PE x86/x64, Mach-O
 * x86-64). Unsupported semantics fail without external fallback. Explicit
 * executable selection uses the optional lld-link/ld64 compatibility bridges.
 * PE response files use Windows quoting; Mach-O uses GNU quoting. External
 * /out:, /implib:, /pdb:, /map: and Mach-O -o/-map are staged. Import libraries
 * default to <output-stem>.lib when exports exist. In-memory calls discard
 * auxiliary files unless explicit destination arguments are supplied.
 * Mach-O callers supply -arch and -platform_version (or toolchain equivalents).
 * GNU/Lua linker scripts are not supported by these two backends.
 * The following staging/auxiliary and platform-version rules describe the
 * external bridges, not features of the limited native image emitter.
 * All pointer ownership and diagnostic rules match qld_elf_options_t. */
#define QLD_PE_OPTIONS_VERSION 1
#define QLD_MACHO_OPTIONS_VERSION 1
typedef qld_elf_options_t qld_pe_options_t;
typedef qld_elf_options_t qld_macho_options_t;
QLD_API void qld_pe_options_init(qld_pe_options_t *);
QLD_API void qld_macho_options_init(qld_macho_options_t *);
/* External executable or NULL when that bridge is disabled; not a test of
 * native PE/Mach-O availability. */
QLD_API const char *qld_pe_linker(void);
QLD_API const char *qld_macho_linker(void);
QLD_API int qld_link_pe(const qld_options_t *, const qld_pe_options_t *, char **errmsg);
QLD_API int qld_link_pe_bytes(const qld_options_t *, const qld_pe_options_t *,
    uint8_t **out_bytes, size_t *out_size, char **errmsg);
QLD_API int qld_link_macho(const qld_options_t *, const qld_macho_options_t *, char **errmsg);
QLD_API int qld_link_macho_bytes(const qld_options_t *, const qld_macho_options_t *,
    uint8_t **out_bytes, size_t *out_size, char **errmsg);

/* Validate a linker script without linking. Returns 0 when the script
 * parses, nonzero otherwise (with *errmsg set as in qld_link). */
QLD_API int qld_check_script(const char* text, char** errmsg);

/* Library version string, e.g. "1.0". Never NULL. */
QLD_API const char* qld_version(void);
QLD_API const char* qld_status_string(int status);

/* Release helpers for qld_link / qld_link_bytes diagnostics. */
QLD_API void qld_free_string(char* s);
QLD_API void qld_free_bytes(uint8_t* p, size_t n);

/* Incremental builder API for bindings that prefer many small calls. */
typedef struct qld_linker qld_linker_t;

qld_linker_t* qld_create(void);
void qld_destroy(qld_linker_t* self);
int qld_add_input(qld_linker_t* self, const char* path);
int qld_add_libpath(qld_linker_t* self, const char* path);
int qld_add_library(qld_linker_t* self, const char* name);
/* NULL resets to automatic backend selection. Invalid settings leave the
 * previous configuration intact. Registered script extensions apply here. */
QLD_API int qld_set_elf_options(qld_linker_t *, const qld_elf_options_t *, char **errmsg);
/* A successful setter selects that backend, replacing previous backend
 * settings. NULL resets to automatic selection; invalid updates are atomic. */
QLD_API int qld_set_pe_options(qld_linker_t *, const qld_pe_options_t *, char **errmsg);
QLD_API int qld_set_macho_options(qld_linker_t *, const qld_macho_options_t *, char **errmsg);
void qld_set_output(qld_linker_t* self, const char* path);
void qld_set_entry(qld_linker_t* self, const char* symbol);
void qld_set_script_path(qld_linker_t* self, const char* path);
void qld_set_script_text(qld_linker_t* self, const char* text);
void qld_set_base(qld_linker_t* self, uint64_t base);
void qld_set_mode(qld_linker_t* self, qld_mode_t mode);
void qld_set_flag(qld_linker_t* self, const char* flag, int value);
int qld_run(qld_linker_t* self, char** errmsg);
int qld_run_bytes(qld_linker_t* self, uint8_t** out_bytes, size_t* out_size,
                   char** errmsg);

/* Source extension SDK v1. Handlers emit source through a bounded sink, return
 * zero on success, and write a diagnostic into error on failure. User data is
 * borrowed until destroy; registrations survive run calls. Names are invoked
 * with @name on their own script line. Arguments retain source quoting. */
#define QLD_SCRIPT_SDK_VERSION 1
typedef int (*qld_source_sink)(void *, const char *, size_t);
typedef int (*qld_directive_handler)(void *user, const char *const *arguments,
    size_t count, qld_source_sink emit, void *sink, char *error, size_t error_size);
int qld_register_directive(qld_linker_t *, const char *name,
    qld_directive_handler, void *user, char **errmsg);
int qld_define_macro(qld_linker_t *, const char *name,
    const char *const *parameters, size_t count, const char *body, char **errmsg);
/* Returned text is owned and freed with qld_free_string. --!lua selects Lua. */
int qld_expand_script(qld_linker_t *, const char *source, char **text, char **errmsg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QOBJLD_LD_H */
