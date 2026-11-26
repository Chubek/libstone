#ifndef ASDL_ASDL_H
#define ASDL_ASDL_H

#include <stddef.h>
#include <stdint.h>

/* ASDL_SHARED is set by the shared-library CMake target; static use does not
 * need an import or export decoration. */
#ifndef ASDL_API
#  if defined(_WIN32) && defined(ASDL_SHARED)
#    if defined(ASDL_BUILDING_LIBRARY)
#      define ASDL_API __declspec(dllexport)
#    else
#      define ASDL_API __declspec(dllimport)
#    endif
#  elif defined(__GNUC__) || defined(__clang__)
#    define ASDL_API __attribute__((visibility("default")))
#  else
#    define ASDL_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ABI version = (major << 16) | minor. Compatible additions increment minor;
 * a breaking ABI change requires a new major. The function returns the
 * loaded library's version, which may differ from this header's version.
 */
#define ASDL_ABI_VERSION UINT32_C(0x00010000)
ASDL_API uint32_t asdl_abi_version(void);

typedef struct asdl_schema asdl_schema;
typedef struct asdl_buffer asdl_buffer;
typedef struct asdl_error asdl_error;

typedef int32_t asdl_status;
enum {
    ASDL_OK = 0,
    ASDL_INVALID_ARGUMENT = 1,
    ASDL_IO_ERROR = 2,
    ASDL_SCHEMA_ERROR = 3,
    ASDL_OUT_OF_MEMORY = 4,
    ASDL_INTERNAL_ERROR = 5
};

typedef uint32_t asdl_dot_view;
enum {
    ASDL_DOT_SCHEMA = 0,
    ASDL_DOT_DEPENDENCIES = 1,
    ASDL_DOT_IMPORTS = 2
};

/*
 * Parse and validate ASDL. asdl_parse reads exactly length bytes (source can
 * be NULL only if length is zero); source_name is an optional NUL-terminated
 * diagnostic label. asdl_load reads the named file.
 *
 * The `out_schema` and `out_buffer` slots are required. When supplied, output
 * slots are overwritten (do not pass slots that still own live objects).
 * A result is returned only on success. Optional `out_error` receives an
 * owned diagnostic on failure if one can be allocated; the returned status
 * is authoritative even if the error is NULL. No exception crosses this API.
 *
 * Schema and buffer objects are independent of each other and of input
 * storage. Dispose of all returned objects using their matching destroy
 * functions, never with free(). Destruction accepts NULL. Immutable handles
 * may be read concurrently as long as they are not concurrently destroyed.
 */
ASDL_API asdl_status asdl_parse(const char *source, size_t length,
                              const char *source_name, asdl_schema **out_schema,
                              asdl_error **out_error);
ASDL_API asdl_status asdl_load(const char *path, asdl_schema **out_schema,
                             asdl_error **out_error);
ASDL_API void asdl_schema_destroy(asdl_schema *schema);

/* Borrowed NUL-terminated strings, valid until the schema is destroyed.
 * A NULL handle or an out-of-range index returns NULL (count returns zero).
 * Types are indexed in declaration order. */
ASDL_API const char *asdl_schema_module(const asdl_schema *schema);
ASDL_API size_t asdl_schema_type_count(const asdl_schema *schema);
ASDL_API const char *asdl_schema_type_name(const asdl_schema *schema, size_t index);

ASDL_API asdl_status asdl_schema_dot(const asdl_schema *schema, asdl_dot_view view,
                                   asdl_buffer **out_buffer, asdl_error **out_error);
/* Borrowed NUL-terminated bytes; the size excludes the terminator. */
ASDL_API const char *asdl_buffer_data(const asdl_buffer *buffer);
ASDL_API size_t asdl_buffer_size(const asdl_buffer *buffer);
ASDL_API void asdl_buffer_destroy(asdl_buffer *buffer);

/* Borrowed NUL-terminated strings, valid until the error is destroyed.
 * Source may be NULL. Position is one-based, or zero if unavailable. */
ASDL_API const char *asdl_error_message(const asdl_error *error);
ASDL_API const char *asdl_error_source(const asdl_error *error);
ASDL_API size_t asdl_error_line(const asdl_error *error);
ASDL_API size_t asdl_error_column(const asdl_error *error);
ASDL_API void asdl_error_destroy(asdl_error *error);

#ifdef __cplusplus
}
#endif

#endif /* ASDL_ASDL_H */
