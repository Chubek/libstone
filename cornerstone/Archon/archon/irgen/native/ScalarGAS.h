#ifndef ARCHON_SCALAR_GAS_H
#define ARCHON_SCALAR_GAS_H

#include "NativeInternal.h"

typedef struct {
    unsigned argc, fp_argc, stack_entry_remainder, stack_slot_bytes, stack_arg_offset;
    const char *args[8], *result, *sp, *link;
} ANGasABI;
extern const ANGasABI an_gas_abi;

typedef struct {
    ANOut *out;
    size_t function;
    unsigned next_label;
    const char *frame_base;
    int base_save;
} ANGas;

void an_gas_print(ANGas *, const char *, ...);
void an_gas_label(ANGas *, unsigned);

/* Each emitter chooses two distinct caller-clobbered operand registers. All
 * value homes are stack slots. Unless otherwise indicated, operations below
 * take inputs in a/b and produce results in a. Stack moves may clobber an
 * additional dedicated address scratch, but not other operand or ABI regs. */
extern const char an_gas_a[], an_gas_b[], an_gas_call_target[];
void an_gas_enter(ANGas *, unsigned frame_size, int frame_record);
void an_gas_leave(ANGas *, unsigned frame_size, int frame_record);
void an_gas_stack_load(ANGas *, const char *dst, unsigned offset);
void an_gas_stack_store(ANGas *, const char *src, unsigned offset);
void an_gas_stack_alloc(ANGas *, unsigned outgoing);
void an_gas_stack_free(ANGas *);
/* Move between an FP ABI register and an eight-byte MIR home. Register
 * number zero is also the result register. Saving f32 clears excess bits. */
void an_gas_fp_load(ANGas *, ArchonMIRType, unsigned reg, unsigned offset);
void an_gas_fp_store(ANGas *, ArchonMIRType, unsigned reg, unsigned offset);
void an_gas_local_addr(ANGas *, unsigned offset);
enum { AN_AMD64_VA_SAVE_BYTES = 48 + 8 * 16 };
void an_amd64_save_varargs(ANGas *, unsigned save, unsigned gp, unsigned fp);
void an_amd64_va_start(ANGas *, unsigned save, unsigned overflow, unsigned gp, unsigned fp);
enum { AN_AARCH64_VA_GP_BYTES = 8 * 8, AN_AARCH64_VA_SAVE_BYTES = AN_AARCH64_VA_GP_BYTES + 8 * 16 };
void an_aarch64_save_varargs(ANGas *, unsigned save, unsigned gp, unsigned fp);
void an_aarch64_va_start(ANGas *, unsigned save, unsigned overflow, unsigned gp, unsigned fp);
void an_gas_symbol_addr(ANGas *, const char *symbol);
void an_gas_constant(ANGas *, uint64_t);
void an_gas_convert(ANGas *, ArchonMIROpcode, unsigned width);
void an_gas_shift_left_imm(ANGas *, unsigned shift);
void an_gas_memory(ANGas *, ArchonMIROpcode, int displacement);
int an_gas_binary(ANGas *, ArchonMIROpcode);
/* Operands and result use the existing integer scratch registers only for
 * bit transport to/from target hardware FP. */
void an_gas_float(ANGas *, ArchonMIROpcode, ArchonMIRType src, ArchonMIRType dst);
/* AMD64 x87 inputs/results are 10-byte memory payloads except for the
 * scalar sides of conversions and the integer result of comparisons. */
void an_amd64_x87(ANGas *, ArchonMIROpcode, ArchonMIRType b, ArchonMIRType dst);
void an_gas_call(ANGas *, const char *symbol, unsigned fp_args);
void an_gas_call_indirect(ANGas *, unsigned fp_args);
void an_gas_jump(ANGas *, unsigned label);
/* Only used for short forward helper branches, not arbitrary CFG targets. */
void an_gas_branch_nonzero(ANGas *, unsigned label);
void an_gas_trap(ANGas *);

#endif
