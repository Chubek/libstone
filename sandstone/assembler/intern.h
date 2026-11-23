/* Sandstone assembler internals (not public API). */
#ifndef SANDSTONE_ASM_INTERN_H
#define SANDSTONE_ASM_INTERN_H

#include <stddef.h>
#include <stdint.h>

#include "assembler.h"
#include "sandstone.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DomSymAs_Bank DomSymAs_Bank;

typedef enum {
    SAND_OP_REG,  /* register: ival = bank enc_num */
    SAND_OP_IMM,  /* integer literal: ival = value */
    SAND_OP_SYM,  /* symbol/label reference: sval = name */
    SAND_OP_MEM,  /* memory expression text: sval = "[...]" */
    SAND_OP_COND  /* condition code: ival = numeric code */
} sand_op_kind_t;

typedef struct {
    sand_op_kind_t kind;
    int64_t ival;
    char *sval; /* owned, for SYM/MEM */
    /* Normalized AMD64 AT&T addressing. The bank selects the opcode and
     * checks widths; these fields describe only the concrete operand. */
    int att, width, indirect, modrm, sib, rex_b, rex_x, disp_size, fixup;
    int64_t disp;
} sand_operand_t;

typedef enum {
    SAND_LINE_EMPTY,
    SAND_LINE_INSN,
    SAND_LINE_DIRECTIVE
} sand_line_kind_t;

typedef struct {
    sand_line_kind_t kind;
    char *label; /* owned, optional label defined on this line */
    char *mnemonic; /* owned: insn mnemonic or directive name (with dot) */
    sand_operand_t *operands;
    size_t n_operands;
    char *raw; /* owned: source text for diagnostics */
    unsigned lineno;
    const char *src_name;
    /* assembly results */
    uint32_t section_idx;
    uint64_t address;
    uint32_t size;
    uint32_t form_id;
    int has_form;
    /* A bank form selected by typed AArch64 GAS operand constraints. */
    uint32_t a64_form_plus_one;
    int64_t a64_disp;
} sand_line_t;

int sand_a64_operands(sandstone_asm_t *, sand_line_t *, char **, size_t);
int sand_a64_bindings(const sand_line_t *, DomSymAs_OpVal **, uint32_t *);
int sand_a64_patch_local(uint8_t *, int64_t, const char *);

typedef struct {
    char *name; /* owned */
    uint8_t *bytes;
    size_t len, cap;
    uint64_t base;
    int is_bss;
    int elf_flags, elf_flags_set;
    uint32_t align;
} sand_section_t;

typedef struct {
    char *name; /* owned */
    char *section; /* owned (may be "" for undefined) */
    uint32_t sec_idx;
    uint64_t address;
    int global;
    int hidden; /* ELF STV_HIDDEN */
    int explicit_local;
    int defined;
    int round; /* pass-1 round that (re)defined this symbol */
} sand_symbol_t;

typedef struct {
    uint32_t sec_idx;
    uint32_t offset;
    char *symbol; /* owned */
    int64_t addend;
    char *type; /* owned */
    uint32_t width_bits;
    int pcrel;
} sand_reloc_intern_t;

struct sandstone_asm {
    DomSymAs_Bank *bank;
    char *syntax_name; /* owned */
    uint32_t syntax_id;
    /* cached syntax parameters */
    char *comment;
    char *reg_prefix;
    char *imm_prefix;
    char *separator;
    uint32_t operand_order; /* 0 normal, 1 reversed */
    /* enabled feature ids for form resolution */
    uint32_t *enabled;
    size_t n_enabled;
    int byte_order; /* 0 LE, 1 BE (from first target) */
    int relocatable;

    sand_line_t *lines;
    size_t n_lines, cap_lines;

    sand_section_t *sections;
    size_t n_sections;

    sand_symbol_t *symbols;
    size_t n_symbols;

    sand_reloc_intern_t *relocs;
    size_t n_relocs;

    /* public-view caches (rebuilt on demand) */
    uint8_t *image;
    size_t image_len;
    int image_valid;
    sandstone_reloc_t *pub_relocs;
    sandstone_sym_t *pub_syms;

    char *hook_path; /* optional Lua preprocess script */
    struct sand_asm_opt_hook *asm_opt_hook; /* owned post-expansion Lua script */
    void *lua_state; /* persistent state shared by Lua blocks in one source */
    void *extensions; /* SDK registrations and optional ArchML profile */
    char error[2048];
};

/* error helper */
int sand_emit_elf64(sandstone_asm_t *, const char *path);
int sand_emit_native_object(sandstone_asm_t *, const char *path, const char *format);
void sand_set_error(sandstone_asm_t *ctx, const char *fmt, ...);

/* parser.c */
char *sand_rv_gas(sandstone_asm_t *ctx, const char *src, const char *name);
int sand_parse_source(sandstone_asm_t *ctx, const char *src,
                      const char *src_name);
char *sand_preprocess_source(sandstone_asm_t *ctx, const char *src,
                             const char *src_name);
char *sand_run_lua_block(sandstone_asm_t *ctx, const char *code);
void sand_lua_reset(sandstone_asm_t *ctx);
int sand_lua_expand(sandstone_asm_t *, const char *, const char *, char **);
void sand_lua_constant(sandstone_asm_t *, const char *, int64_t);
int sand_extension_expand(sandstone_asm_t *, const char *, const char *, char **);
char *sand_extension_prelude(sandstone_asm_t *);
void sand_extension_free(sandstone_asm_t *);
int sand_extension_reserved(const char *);
void sand_free_lines(sandstone_asm_t *ctx);
/* classify helpers exposed for directives.c */
int sand_parse_integer(const char *text, int64_t *out);
char *sand_trim(char *s);
int sand_is_name(const char *s);

/* directives.c: size of a data directive line (pass 1). */
int sand_directive_size(sandstone_asm_t *ctx, const sand_line_t *line,
                        uint32_t *size_out);
/* directives.c: emit a data directive line (pass 2). */
int sand_emit_directive(sandstone_asm_t *ctx, const sand_line_t *line,
                        uint32_t sec_idx, uint64_t address);
/* directives.c: alignment/origin padding (pass 1 sizing + pass 2 emit). */
int sand_directive_pad(sandstone_asm_t *ctx, const sand_line_t *line,
                       uint32_t sec_idx, uint64_t address, int emit);
/* directives.c: section-switch and symbol directives. */
int sand_directive_section(const sand_line_t *line, const char **name_out,
                           int *is_bss_out);
int sand_directive_is_symbol(const sand_line_t *line);
int sand_directive_symbol(sandstone_asm_t *ctx, const sand_line_t *line,
                          int round);

/* binarizer.c */
int sand_assemble_lines(sandstone_asm_t *ctx);
int sand_encode_literal(const sandstone_asm_t *ctx, const char *source,
                        uint8_t *out, size_t size);

/* lua.cpp / asm_opt.cpp */
char *sand_run_hook(sandstone_asm_t *ctx, const char *source,
                    const char *src_name);
char *sand_run_asm_opt_hook(sandstone_asm_t *ctx, const char *source,
                            const char *src_name);

/* shared small utilities (assembler.c) */
char *sand_strdup(const char *s);
char *sand_strndup(const char *s, size_t n);
void *sand_realloc_array(void *ptr, size_t n, size_t elem);
sand_symbol_t *sand_find_symbol(sandstone_asm_t *ctx, const char *name);
sand_section_t *sand_find_section(sandstone_asm_t *ctx, const char *name);
sand_section_t *sand_require_section(sandstone_asm_t *ctx, const char *name);
int64_t sand_eval_expr(sandstone_asm_t *ctx, const char *expr, int *ok);

#ifdef __cplusplus
}
#endif
#endif /* SANDSTONE_ASM_INTERN_H */
