/* Sandstone assembler framework (C API).
 *
 * Instruction forms and encoding recipes come from built-in DSymspec
 * profiles or from an explicitly supplied `.symas` databank.
 *
 * Assembly model (two passes, single section cursor):
 *  - pass 1 parses each line, encodes instructions with symbol operands
 *    passed by name (encoders emit placeholder bytes plus relocation
 *    records), and assigns addresses to labels;
 *  - pass 2 re-encodes and resolves relocations whose symbol is a locally
 *    defined label: absolute relocs are patched as S + A, PC-relative
 *    relocs as S + A - P where P is the end of the containing instruction
 *    when the field is a strict suffix of the instruction and the start of
 *    the instruction otherwise (this covers trailing rel32 immediates as
 *    well as whole-instruction PC-relative fields; see binarizer.c).
 *    Relocations against undefined symbols are kept only in `obj` output;
 *    flat (`bin`) output rejects them.
 *
 * Operand-name binding: SymAS operand descriptors are positional while
 * encoding functions reference operands by name (`dst`, `src`, `rd`,
 * `rs1`, `imm`, `sym`, `cond`, ...). The assembler binds every positional
 * operand under each conventional name for its position (operand 0 also as
 * `dst`/`rd`, operand 1 also as `src`/`rs1`, operand 2 also as `rs2`, and
 * immediates also as `imm`, symbols also as `sym`/`target`, conditions also
 * as `cond`). Superfluous bindings are ignored by the encoder.
 *
 * Ownership: the context owns everything; pointers returned by getters are
 * borrowed and valid until the next assemble call or sandstone_asm_free().
 * Error text is available via sandstone_asm_error() after any failing call.
 */
#ifndef SANDSTONE_ASSEMBLER_H
#define SANDSTONE_ASSEMBLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sandstone_asm sandstone_asm_t;

/* One emitted relocation: byte offset within its section, symbol name,
 * addend, relocation type name (bank reloc name, or "ABS" for absolute
 * directive fixups), field width in bits, and PC-relative flag. */
typedef struct sandstone_reloc {
    char *section;
    uint32_t offset;
    char *symbol;
    int64_t addend;
    char *type;
    uint32_t width_bits;
    int pcrel;
} sandstone_reloc_t;

/* One symbol-table entry. `defined` is 0 for externals. */
typedef struct sandstone_sym {
    char *name;
    char *section;
    uint64_t address;
    int global;
    int defined;
} sandstone_sym_t;

/* Lifecycle. err/err_n receive a diagnostic on failure (may be NULL).
 * A NULL bank_path selects the build's default DSymspec profile. */
int sandstone_asm_new(sandstone_asm_t **out, const char *bank_path,
                      char *err, size_t err_n);
int sandstone_asm_new_from_memory(sandstone_asm_t **out, const uint8_t *data,
                                  size_t len, char *err, size_t err_n);
void sandstone_asm_free(sandstone_asm_t *ctx);

/* Select the syntax record by name (default: first syntax in the bank).
 * Passing NULL or "" keeps the current selection. */
int sandstone_asm_set_syntax(sandstone_asm_t *ctx, const char *name);

/* Enable an architectural feature by name (for form resolution).
 * By default the target's own feature list is enabled. */
int sandstone_asm_enable_feature(sandstone_asm_t *ctx, const char *name);
int sandstone_asm_disable_feature(sandstone_asm_t *ctx, const char *name);

/* Assemble source text (or a file). On success the output sections, bytes,
 * relocations and symbols are available through the getters below. */
int sandstone_asm_assemble_string(sandstone_asm_t *ctx, const char *src,
                                  const char *src_name);
int sandstone_asm_assemble_file(sandstone_asm_t *ctx, const char *path);
/* Assemble and write a little-endian ELF64 relocatable object. Selects
 * relocatable layout before processing source (unlike flat image layout). */
int sandstone_asm_assemble_elf(sandstone_asm_t *ctx, const char *src,
                               const char *src_name, const char *output);
/* Native relocatable object; format is "elf", "coff" or "macho". */
int sandstone_asm_assemble_object(sandstone_asm_t *ctx, const char *src,
                                  const char *src_name, const char *output, const char *format);

/* Output model: sections in order of first use. */
uint32_t sandstone_asm_section_count(const sandstone_asm_t *ctx);
const char *sandstone_asm_section_name(const sandstone_asm_t *ctx,
                                       uint32_t i);
const uint8_t *sandstone_asm_section_bytes(const sandstone_asm_t *ctx,
                                           uint32_t i, size_t *len_out);
uint64_t sandstone_asm_section_base(const sandstone_asm_t *ctx, uint32_t i);

/* Flat image: alloc sections concatenated in order (BSS zero-filled). */
const uint8_t *sandstone_asm_image(const sandstone_asm_t *ctx,
                                   size_t *len_out);

/* Relocations and symbols (borrowed arrays). */
const sandstone_reloc_t *sandstone_asm_relocs(const sandstone_asm_t *ctx,
                                              uint32_t *n_out);
const sandstone_sym_t *sandstone_asm_symbols(const sandstone_asm_t *ctx,
                                            uint32_t *n_out);

/* Write outputs. `format` is "bin" (flat image) or "obj" (JSON object file
 * with sections, symbols and relocations). Returns 0 on success. */
int sandstone_asm_write(const sandstone_asm_t *ctx, const char *path,
                        const char *format);

/* Last error for this context (never NULL). */
const char *sandstone_asm_error(const sandstone_asm_t *ctx);

/* Optional Lua preprocessing hook: run `source` through a user preprocessing
 * script. Without a script this is the identity function. Returns a
 * malloc'd string the caller owns, or NULL on error (see error()). */
char *sandstone_asm_preprocess(sandstone_asm_t *ctx, const char *source);
int sandstone_asm_load_hook(sandstone_asm_t *ctx, const char *lua_path);

/* Optional post-expansion Lua hook. Set/load copy a nonempty Lua text chunk
 * (max 16 MiB) and check its syntax without executing it. They return 0 on
 * success, or -1 on failure, leaving the old hook in place. Each assembly
 * runs the chunk in a fresh state, calls its global asm_optimize(source, name)
 * once, and parses the complete string it returns without re-expansion. The
 * result may not contain NUL bytes or exceed 16 MiB. This does not affect
 * sandstone_asm_preprocess(). Clear disables the hook. */
int sandstone_asm_set_asm_opt_hook(sandstone_asm_t *ctx, const char *lua_source,
                                    size_t lua_size);
int sandstone_asm_load_asm_opt_hook(sandstone_asm_t *ctx, const char *lua_path);
void sandstone_asm_clear_asm_opt_hook(sandstone_asm_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SANDSTONE_ASSEMBLER_H */
