/**
 * @file memtkx/core/base.h
 * @brief Export macros, language glue, status codes, panic and allocator
 *        hooks shared by every MemTKX header.
 *
 * MemTKX is a C99 library. Every public header can also be included from
 * C++ (declarations are wrapped in `extern "C"`).
 */
#ifndef MEMTKX_CORE_BASE_H
#define MEMTKX_CORE_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "memtkx/core/version.h"

/* ------------------------------------------------------------------ */
/*  Language glue                                                     */
/* ------------------------------------------------------------------ */

#ifdef __cplusplus
#define MTKX_BEGIN_DECLS extern "C" {
#define MTKX_END_DECLS }
#else
#define MTKX_BEGIN_DECLS
#define MTKX_END_DECLS
#endif

/* ------------------------------------------------------------------ */
/*  Symbol visibility                                                 */
/*                                                                    */
/*  MTKX_SHARED is defined (publicly, by the CMake target) when       */
/*  MemTKX is built as a shared library. MTKX_BUILDING_LIBRARY is     */
/*  defined only while compiling the library itself.                  */
/* ------------------------------------------------------------------ */

#if defined(MTKX_SHARED)
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(MTKX_BUILDING_LIBRARY)
#define MTKX_API __declspec(dllexport)
#else
#define MTKX_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define MTKX_API __attribute__((visibility("default")))
#else
#define MTKX_API
#endif
#else
#define MTKX_API
#endif

/* ------------------------------------------------------------------ */
/*  Compiler helpers                                                  */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
#define MTKX_NORETURN __attribute__((noreturn))
#define MTKX_NODISCARD __attribute__((warn_unused_result))
#define MTKX_UNUSED __attribute__((unused))
#elif defined(_MSC_VER)
#define MTKX_NORETURN __declspec(noreturn)
#define MTKX_NODISCARD
#define MTKX_UNUSED
#else
#define MTKX_NORETURN
#define MTKX_NODISCARD
#define MTKX_UNUSED
#endif

/** `inline` is a keyword in C99 and C++; MSVC's C mode spells it __inline. */
#if defined(_MSC_VER) && !defined(__cplusplus)
#define MTKX_INLINE static __inline
#else
#define MTKX_INLINE static inline
#endif

MTKX_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Status codes                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Generic status for operations that are not allocation or
 *        collection results (initialisation, container growth, I/O ...).
 */
typedef enum mtkx_status {
  MTKX_OK = 0,                    /**< success */
  MTKX_ERR_NOMEM = 1,             /**< native-heap allocation failed */
  MTKX_ERR_INVALID_ARGUMENT = 2,  /**< an argument violated a precondition */
  MTKX_ERR_NOT_FOUND = 3,         /**< a lookup failed */
  MTKX_ERR_DUPLICATE = 4,         /**< an item with this identity exists */
  MTKX_ERR_PARSE = 5,             /**< text could not be parsed */
  MTKX_ERR_IO = 6,                /**< an operating-system I/O call failed */
  MTKX_ERR_SYSTEM = 7,            /**< another operating-system call failed */
  MTKX_ERR_RANGE = 8              /**< a value is outside the representable range */
} mtkx_status;

/** @return a static, human-readable description of @p status. */
MTKX_API const char *mtkx_status_string(mtkx_status status);

/* ------------------------------------------------------------------ */
/*  Panic                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Handler invoked for unrecoverable misuse, e.g. unwrapping an
 *        error result. It must not return; if it does, abort() is called.
 */
typedef void (*mtkx_panic_handler)(const char *message);

/** Installs @p handler (NULL restores the default) and returns the previous one. */
MTKX_API mtkx_panic_handler mtkx_set_panic_handler(mtkx_panic_handler handler);

/** Reports @p message through the panic handler and terminates. */
MTKX_API MTKX_NORETURN void mtkx_panic(const char *message);

/* ------------------------------------------------------------------ */
/*  Native-heap hooks                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Functions MemTKX uses for its own bookkeeping memory (object
 *        tables, free-cell arrays, strings, owned heap buffers ...).
 *
 * Install replacements with mtkx_set_allocator() before creating any
 * MemTKX object; memory must be released by the same hooks that
 * allocated it. Any NULL member falls back to the C library function.
 */
typedef struct mtkx_allocator_hooks {
  void *(*malloc_fn)(size_t size);
  void *(*calloc_fn)(size_t count, size_t size);
  void *(*realloc_fn)(void *ptr, size_t size);
  void (*free_fn)(void *ptr);
} mtkx_allocator_hooks;

/** Installs allocator hooks; NULL restores the C library defaults. */
MTKX_API void mtkx_set_allocator(const mtkx_allocator_hooks *hooks);

/** Allocates @p size bytes with the active hooks. */
MTKX_API void *mtkx_malloc(size_t size);
/** Allocates zeroed memory with the active hooks. */
MTKX_API void *mtkx_calloc(size_t count, size_t size);
/** Reallocates with the active hooks. */
MTKX_API void *mtkx_realloc(void *ptr, size_t size);
/** Releases memory returned by MemTKX (strings, dumps, buffers). */
MTKX_API void mtkx_free(void *ptr);
/** Duplicates a NUL-terminated string with the active hooks (NULL on OOM). */
MTKX_API char *mtkx_strdup(const char *text);
/** Duplicates @p length bytes of @p text and appends a NUL (NULL on OOM). */
MTKX_API char *mtkx_strndup(const char *text, size_t length);

/** @return the runtime library version (compare with MTKX_VERSION). */
MTKX_API int mtkx_version(void);

MTKX_END_DECLS

#endif /* MEMTKX_CORE_BASE_H */
