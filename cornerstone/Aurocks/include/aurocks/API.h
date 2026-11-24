#ifndef AUROCKS_API_H
#define AUROCKS_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aurocks_result aurocks_result;

/* Zero-initialize before assigning fields. NULL strings use grammar/default
 * settings; lua_source, when supplied, is sandboxed compile-time source. */
typedef struct aurocks_options {
    const char *entrypoint;
    const char *backend;
    const char *algorithm;
    const char *disambiguator;
    const char *lua_source;
    int check_only;
} aurocks_options;

typedef enum aurocks_severity {
    AUROCKS_WARNING,
    AUROCKS_ERROR
} aurocks_severity;

typedef struct aurocks_diagnostic {
    aurocks_severity severity;
    size_t offset;
    size_t line;
    size_t column;
    const char *code;
    const char *message;
} aurocks_diagnostic;

typedef struct aurocks_analysis {
    const char *backend;
    const char *algorithm;
    const char *disambiguator;
    size_t states;
    size_t conflicts;
} aurocks_analysis;

/* The returned result is owned by the caller. NULL means invalid arguments
 * or a host failure (e.g. allocation failure), not a grammar diagnostic. */
aurocks_result *aurocks_compile(const char *grammar, size_t length,
                               const aurocks_options *options);
void aurocks_result_destroy(aurocks_result *result);
int aurocks_result_ok(const aurocks_result *result);
/* Borrowed, NUL-terminated source; may be empty for check-only or errors. */
const char *aurocks_result_source(const aurocks_result *result, size_t *length);
size_t aurocks_result_diagnostic_count(const aurocks_result *result);
/* Return nonzero on success. Strings are borrowed until result destruction. */
int aurocks_result_analysis(const aurocks_result *result, aurocks_analysis *analysis);
/* Return nonzero on success. Strings are borrowed until result destruction. */
int aurocks_result_diagnostic(const aurocks_result *result, size_t index,
                             aurocks_diagnostic *diagnostic);

#ifdef __cplusplus
}
#endif

#endif
