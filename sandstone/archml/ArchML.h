#ifndef SANDSTONE_ARCHML_H
#define SANDSTONE_ARCHML_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct sandstone_archml_document sandstone_archml_document;
typedef struct sandstone_archml_error {
    size_t offset, line, column;
    char message[512];
} sandstone_archml_error;
typedef enum sandstone_archml_status {
    SANDSTONE_ARCHML_OK = 0,
    SANDSTONE_ARCHML_INVALID = 1,
    SANDSTONE_ARCHML_BUFFER_TOO_SMALL = 2,
    SANDSTONE_ARCHML_NO_MATCH = 3
} sandstone_archml_status;

/* Documents own their data. Borrowed names live until document_free. Errors are
 * optional, caller-owned and cleared on success. No C++ exception crosses this API. */
sandstone_archml_document *sandstone_archml_parse(const char *text, size_t length, sandstone_archml_error *error);
sandstone_archml_document *sandstone_archml_load(const char *path, sandstone_archml_error *error);
void sandstone_archml_document_free(sandstone_archml_document *document);
const char *sandstone_archml_name(const sandstone_archml_document *document);
size_t sandstone_archml_instruction_count(const sandstone_archml_document *document);
const char *sandstone_archml_instruction_name(const sandstone_archml_document *document, size_t index);

/* Generated text and record JSON are allocated by this library; free with
 * sandstone_archml_free. Output arguments are unchanged on error. */
sandstone_archml_status sandstone_archml_generate(const sandstone_archml_document *, const char *target,
    const char *namespace_name, char **output, size_t *length, sandstone_archml_error *error);
sandstone_archml_status sandstone_archml_find(const sandstone_archml_document *, const char *kind,
    const char *name, char **json, size_t *length, sandstone_archml_error *error);
void sandstone_archml_free(void *memory);

/* Signed operands are uint64_t two's-complement values; displacement values are
 * bytes. On BUFFER_TOO_SMALL only the length/count changes (required size).
 * Encode/decode leave buffers unchanged on every failure. Decode consumes the
 * first matching form in source order; feature/mode selection is caller-owned. */
sandstone_archml_status sandstone_archml_encode(const sandstone_archml_document *, const char *name,
    const uint64_t *operands, size_t operand_count, uint8_t *output, size_t *length,
    sandstone_archml_error *error);
sandstone_archml_status sandstone_archml_decode(const sandstone_archml_document *, const uint8_t *bytes,
    size_t length, size_t *instruction_index, uint64_t *operands, size_t *operand_count,
    size_t *consumed, sandstone_archml_error *error);
#ifdef __cplusplus
}
#endif
#endif
