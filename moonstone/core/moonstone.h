#ifndef MOONSTONE_CORE_V1_H
#define MOONSTONE_CORE_V1_H

#include "Export.hpp"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes are uint32_t, not implementation-dependent C enum sizes. */
typedef uint32_t ms_v1_status;
#define MS_V1_OK UINT32_C(0)
#define MS_V1_INVALID_INPUT UINT32_C(1)
#define MS_V1_UNSUPPORTED UINT32_C(2)
#define MS_V1_BUDGET_EXHAUSTED UINT32_C(3)
#define MS_V1_NOT_FOUND UINT32_C(4)
#define MS_V1_OUT_OF_MEMORY UINT32_C(5)
#define MS_V1_INTERNAL_ERROR UINT32_C(6)

typedef struct ms_v1_source ms_v1_source;
typedef struct ms_v1_span {
    uint32_t line, column, end_line, end_column;
} ms_v1_span;

MOONSTONE_API uint32_t MOONSTONE_C_CALL ms_v1_api_version(void);
/* Immutable source. Input strings are copied; lengths exclude any terminator.
 * NULL is valid only for zero-length input. max_bytes bounds file+text bytes.
 * On failure *out_source is NULL. No exceptions cross this ABI. */
MOONSTONE_API ms_v1_status MOONSTONE_C_CALL ms_v1_source_create(const char *file, size_t file_size,
                                                                const char *text, size_t text_size,
                                                                size_t max_bytes,
                                                                ms_v1_source **out_source);
/* Half-open UTF-8 byte offsets; output coordinates are one-based BYTE columns.
 * Failure clears *out_span. A live source permits concurrent read calls. */
MOONSTONE_API ms_v1_status MOONSTONE_C_CALL ms_v1_source_span(const ms_v1_source *source,
                                                              size_t begin, size_t end,
                                                              ms_v1_span *out_span);
/* NULL is accepted. Destruction must not race any access to this handle. */
MOONSTONE_API void MOONSTONE_C_CALL ms_v1_source_destroy(ms_v1_source *source);

#ifdef __cplusplus
}
#endif
#endif
