#ifndef ARCHON_SCALAR_GAS_H
#define ARCHON_SCALAR_GAS_H

#include "NativeInternal.h"

typedef struct {
    unsigned argc, stack_entry_remainder, stack_slot_bytes, stack_arg_offset;
    const char *args[8], *result, *sp, *link;
} ANGasABI;
extern const ANGasABI an_gas_abi;

typedef struct {
    ANOut *out;
    size_t function;
    unsigned next_label;
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
void an_gas_local_addr(ANGas *, unsigned offset);
void an_gas_symbol_addr(ANGas *, const char *symbol);
void an_gas_constant(ANGas *, uint64_t);
void an_gas_convert(ANGas *, ArchonMIROpcode, unsigned width);
void an_gas_memory(ANGas *, ArchonMIROpcode, int displacement);
int an_gas_binary(ANGas *, ArchonMIROpcode);
void an_gas_call(ANGas *, const char *symbol);
void an_gas_call_indirect(ANGas *);
void an_gas_jump(ANGas *, unsigned label);
/* Only used for short forward helper branches, not arbitrary CFG targets. */
void an_gas_branch_nonzero(ANGas *, unsigned label);
void an_gas_trap(ANGas *);

#endif
