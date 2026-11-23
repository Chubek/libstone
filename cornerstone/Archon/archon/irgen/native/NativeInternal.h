#ifndef ARCHON_NATIVE_INTERNAL_H
#define ARCHON_NATIVE_INTERNAL_H

#include "Backend.h"
#include <stdint.h>

#define AN_MAX_VALUE 4096u
#define AN_MAX_BLOCK ARCHON_MIR_MAX_BLOCKS
#define AN_MAX_PARAMS ARCHON_MIR_MAX_PARAMS
#define AN_MAX_INSTS 16384u
#define AN_MAX_MODULE_INSTS 65536u
#define AN_MAX_SYMBOLS 256u
#define AN_ZERO 0u
#define AN_RA 1u
#define AN_SP 2u
#define AN_T0 5u
#define AN_T1 6u
#define AN_T2 7u
#define AN_A0 10u
#define AN_A7 17u
#define AN_T3 28u
#define AN_T4 29u
#define AN_T6 31u

typedef struct ANLayout ANLayout;
typedef struct { uint64_t zero, one; } ANProvenBits;
typedef struct {
    char *data;
    size_t size, capacity, limit;
    ArchonBackendStatus status; /* First failure sticky. */
    size_t instructions;
    int physical;
    int machine_code;
    int pic;
    ANLayout *layout;
    const char *detail;
} ANOut;
typedef struct {
    const ArchonBackendOptions *options;
    ArchonBackendDiagnostic *diag;
    ArchonBackendStage stage;
    size_t function, block, instruction;
    unsigned flags;
    const ArchonBackendScheduleHints *schedule_hints;
    const ArchonBackendFactHints *facts;
    const ANProvenBits *proven_bits; /* Replayed against original verified SSA. */
} ANContext;

static inline int an_fact_at(const ANContext *ctx, ArchonBackendFactKind kind,
    size_t block, uint32_t key)
{
    return ctx->facts && ctx->facts->contains &&
        ctx->facts->contains(ctx->facts->user, kind, ctx->function, block, key) > 0;
}

static inline int an_fact(const ANContext *ctx, ArchonBackendFactKind kind, uint32_t key)
{
    return an_fact_at(ctx, kind, ctx->block, key);
}

static inline int an_query_known_bits(const ANContext *ctx, ArchonValue value,
    uint64_t *zero, uint64_t *one)
{
    *zero = *one = 0;
    if (!ctx->proven_bits ||
        ctx->facts->known_bits(ctx->facts->user, ctx->function, value, zero, one) <= 0 ||
        (*zero & *one))
        return 0;
    *zero &= ctx->proven_bits[value].zero;
    *one &= ctx->proven_bits[value].one;
    return (*zero | *one) != 0;
}
typedef struct {
    const char *name;
    unsigned argc, args[8], result, stack_align, stack_pointer, return_address;
} ANABI;
extern const ANABI an_abi;

static inline int an_is_call(ArchonMIROpcode opcode)
{
    return opcode == ARCHON_MIR_CALL || opcode == ARCHON_MIR_CALL_INDIRECT;
}

static inline int an_is_float(ArchonMIRType type)
{
    return type == ARCHON_MIR_F32 || type == ARCHON_MIR_F64;
}

static inline int an_is_value_type(ArchonMIRType type)
{
    return type == ARCHON_MIR_I64 || type == ARCHON_MIR_PTR ||
        type == ARCHON_MIR_FUNC_PTR || an_is_float(type) || type == ARCHON_MIR_F80 || type == ARCHON_MIR_F128;
}

typedef enum {
    AN_ADD, AN_SUB, AN_MUL, AN_DIV, AN_REM, AN_DIVU, AN_REMU,
    AN_AND, AN_OR, AN_XOR, AN_SLL, AN_SRL, AN_SRA, AN_SLT, AN_SLTU,
    AN_ADDI, AN_SLLI, AN_LD, AN_SD, AN_JALR, AN_EBREAK,
    AN_BEQ, AN_BNE, AN_AUIPC,
    AN_LB, AN_LBU, AN_LH, AN_LHU, AN_LW, AN_LWU,
    AN_SB, AN_SH, AN_SW, AN_OPCODE_COUNT
} ANOpcode;

typedef struct { unsigned operand, lsb, width, source_lsb; } ANEncodingField;
typedef struct {
    const char *mnemonic;
    uint32_t mask, bits;
    unsigned registers, imm_width, imm_signed, imm_zero_mask, field_count;
    ANEncodingField fields[6];
} ANEncoding;
extern const ANEncoding an_encodings[AN_OPCODE_COUNT];

int an_encode(ANOpcode, unsigned, unsigned, unsigned, int, uint32_t *);
/* Input offsets are instruction ordinals. Encode a two-word PC-relative
 * sequence with an AUIPC anchor and an ADDI or JALR using its result. */
int an_encode_pcrel(size_t from, size_t to, unsigned base, unsigned dst,
    ANOpcode lower, uint32_t words[2]);
void an_store_word(char *, uint32_t);
void an_out_error(ANOut *, ArchonBackendStatus, const char *);
int an_layout_begin(ANOut *, size_t function, const char *symbol);
int an_layout_label(ANOut *, size_t function, unsigned label);
int an_layout_reference(ANOut *, size_t function, unsigned label, unsigned prefix_words);
int an_layout_entry(ANOut *, const char *symbol, size_t *entry);
int an_layout_finish(ANContext *, ANOut *, unsigned label_count);
void an_layout_dispose(ANOut *);

int an_error(ANContext *, ArchonBackendStatus, const char *);
int an_continue(ANContext *);
int an_symbol(const char *);
int an_verify(ANContext *, const ArchonMIRModule *);
int an_compile_module(ANContext *, const ArchonMIRModule *, ANOut *);
ArchonBackendStatus an_compile_with_hints(const ArchonMIRModule *,
    const ArchonBackendOptions *, unsigned, const ArchonBackendScheduleHints *,
    const ArchonBackendFactHints *, ArchonBackendArtifact *, ArchonBackendDiagnostic *);
/* Operand roles: RRR rd,rs1,rs2; ADDI/SLLI rd,rs1,0,imm;
 * loads rd,base,0,disp; stores source,base,0,disp; JALR rd,base,0,disp. */
void an_emit(ANOut *, ANOpcode, unsigned, unsigned, unsigned, int);
void an_label(ANOut *, size_t fn, unsigned label);
void an_jump(ANOut *, size_t fn, unsigned label);
void an_branch(ANOut *, int not_equal, unsigned, unsigned, size_t fn, unsigned label);
void an_call_symbol(ANOut *, const char *);
void an_address_symbol(ANOut *, unsigned dst, const char *);
void an_asm_header(ANOut *);
void an_asm_function(ANOut *, const char *name);
void an_asm_end_function(ANOut *, const char *name);
int an_asm_globals(ANContext *, ANOut *, const ArchonMIRModule *);
void an_asm_footer(ANOut *);
/* Allocator: homes outside [AN_T4, AN_T6] have stack slots, only those need
 * space. ABI parameters and phi destinations always have stack homes.
 * AN_T3 is reserved for frame-address and branch expansion. */
typedef struct { unsigned reg; uint32_t slot; } ANHome;
int an_allocate(ANContext *, const ArchonMIRFunction *, ANHome *, size_t *slots);
int an_verify_homes(ANContext *, const ArchonMIRFunction *, const ANHome *, size_t slots);
int an_schedule_block(ANContext *, const ArchonMIRBlock *, ArchonMIRInst *);
int an_verify_schedule(ANContext *, const ArchonMIRBlock *, const size_t *order, size_t count);
typedef struct {
    int frame_size, slots_base, phi_base, return_offset;
    int va_start_offset; /* -1 if unused, otherwise relative to adjusted SP. */
    int local_offsets[AN_MAX_VALUE];
    const ArchonMIRType *types;
    int high_offsets[AN_MAX_VALUE]; /* Private high words of f128 values. */
} ANFrame;

/* Fixed-prototype LP64 argument cursor, in XLEN slots. A quad can split
 * across a7 and the stack; only an entirely stacked quad needs alignment. */
static inline size_t an_arg_slot(size_t *cursor, int wide)
{
    size_t first = *cursor;
    if (wide && first >= an_abi.argc)
        first = (first + 1) & ~(size_t)1;
    *cursor = first + 1 + wide;
    return first;
}
static inline size_t an_argument_slots(const ArchonMIRType *types, const ArchonValue *args, size_t count)
{
    size_t i, cursor = 0;
    for (i = 0; i < count; ++i)
        an_arg_slot(&cursor, types && types[args ? args[i] : i] == ARCHON_MIR_F128);
    return cursor;
}
static inline size_t an_edge_words(const ArchonMIRType *types, const ArchonValue *args, size_t count)
{
    size_t i, words = count;
    for (i = 0; i < count; ++i)
        words += types[args[i]] == ARCHON_MIR_F128;
    return words;
}
int an_frame(ANContext *, const ArchonMIRFunction *, size_t slots,
    size_t phi_count, size_t outgoing_words, int calls, int variadic, ANFrame *);
int an_verify_frame(ANContext *, const ArchonMIRFunction *, size_t slots,
    size_t phi_count, size_t outgoing_words, int calls, const ANFrame *);
void an_li(ANOut *, unsigned dst, uint64_t value);
void an_addr(ANOut *, unsigned dst, unsigned base, int offset);
void an_load(ANOut *, unsigned dst, unsigned base, int offset);
void an_store(ANOut *, unsigned src, unsigned base, int offset);
void an_enter(ANOut *, const ANFrame *);
void an_leave(ANOut *, const ANFrame *);
void an_read(ANOut *, const ANFrame *, const ANHome *, ArchonValue, unsigned dst);
void an_write(ANOut *, const ANFrame *, const ANHome *, ArchonValue, unsigned src);

/* First block labels [0, block_count); fresh helper labels start at block_count. */
void an_terminator(ANContext *, ANOut *, const ArchonMIRFunction *, const ANFrame *,
    const ANHome *, const ArchonMIRTerm *, unsigned *next_label);
/* ABI moves operate on verified LP64 signatures and already chosen homes. */
size_t an_outgoing_words(size_t argc);
void an_parameters(ANOut *, const ArchonMIRFunction *, const ANFrame *, const ANHome *);
void an_call(ANOut *, const ArchonMIRInst *, const ANFrame *, const ANHome *);
void an_call_indirect(ANOut *, const ArchonMIRInst *, const ANFrame *, const ANHome *);
/* Operation shape: bits 0-2 mean dst,a,b respectively; imm/symbol/args are
 * verified separately according to opcode. Zero means unknown opcode. */
unsigned an_shape(ArchonMIROpcode);
int an_target_type(ANContext *, ArchonMIRType);
int an_legal(ANContext *, const ArchonMIRFunction *, const ArchonMIRInst *);
ANProvenBits *an_prove_known_bits(const ANContext *, const ArchonMIRFunction *);
int an_fact_conversion_is_copy(ANContext *, const ArchonMIRInst *);
ArchonValue an_fact_mul_shift(ANContext *, const ArchonMIRInst *, unsigned *shift);
void an_convert(ANOut *, ArchonMIROpcode, unsigned dst, unsigned src,
    unsigned count_scratch, unsigned width);
/* Selected operation provider; operands use explicit target scratch registers.
 * Handles arithmetic and comparisons; returns 0 only for unsupported opcode. */
int an_select(ANOut *, ArchonMIROpcode, unsigned dst, unsigned a, unsigned b,
    size_t fn, unsigned *next_label);
void an_select_shift_left_imm(ANOut *, unsigned dst, unsigned src, unsigned shift);

#endif
