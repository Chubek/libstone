#ifndef ARCHON_NATIVE_INTERNAL_H
#define ARCHON_NATIVE_INTERNAL_H

#include "Backend.h"
#include <stdint.h>

#define AN_MAX_VALUE 4096u
#define AN_MAX_BLOCK 256u
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
} ANContext;
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
        type == ARCHON_MIR_FUNC_PTR || an_is_float(type);
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
    int local_offsets[AN_MAX_VALUE];
} ANFrame;
int an_frame(ANContext *, const ArchonMIRFunction *, size_t slots,
    size_t phi_count, size_t outgoing_words, int calls, ANFrame *);
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

/* Edges: parallel copies from the pre-edge environment and an explicit jump.
 * First block labels [0, block_count); fresh helper labels start at block_count. */
void an_edge(ANOut *, size_t fn, const ArchonMIRFunction *, const ANFrame *,
    const ANHome *, const ArchonMIREdge *);
void an_terminator(ANOut *, size_t fn, const ArchonMIRFunction *, const ANFrame *,
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
void an_convert(ANOut *, ArchonMIROpcode, unsigned dst, unsigned src,
    unsigned count_scratch, unsigned width);
/* Selected operation provider; operands use explicit target scratch registers.
 * Handles arithmetic and comparisons; returns 0 only for unsupported opcode. */
int an_select(ANOut *, ArchonMIROpcode, unsigned dst, unsigned a, unsigned b,
    size_t fn, unsigned *next_label);

#endif
