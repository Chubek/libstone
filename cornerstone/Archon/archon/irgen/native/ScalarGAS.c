#include "ScalarGAS.h"
#include "TargetNative.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_LIMIT 65536u

/* All MIR values occupy a word, and every region is disjoint. Offsets are
 * nonnegative from the adjusted stack pointer, which remains fixed throughout
 * the body (including during call setup). */
typedef struct {
    unsigned size, homes, copies, locals[AN_MAX_VALUE];
    int record;
} GasFrame;

void an_gas_print(ANGas *gas, const char *format, ...)
{
    ANOut *out = gas->out;
    char line[1024]; /* At most three verified names of up to 255 bytes. */
    va_list args;
    int written;
    size_t count, needed, limit, cap;
    if (out->status != ARCHON_BACKEND_OK)
        return;
    va_start(args, format);
    written = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= sizeof line) {
        out->status = ARCHON_BACKEND_INTERNAL;
        return;
    }
    count = (size_t)written;
    if (count > out->limit - out->size) {
        out->status = ARCHON_BACKEND_LIMIT;
        return;
    }
    needed = out->size + count + 1;
    if (needed > out->capacity) {
        char *data;
        limit = out->limit + 1; /* Backend.c clamps the public limit. */
        cap = out->capacity ? out->capacity : (limit < 256u ? limit : 256u);
        while (cap < needed)
            cap = cap > limit / 2 ? limit : cap * 2;
        data = realloc(out->data, cap);
        if (!data) {
            out->status = ARCHON_BACKEND_NOMEM;
            return;
        }
        out->data = data;
        out->capacity = cap;
    }
    memcpy(out->data + out->size, line, count + 1);
    out->size += count;
}

void an_gas_label(ANGas *gas, unsigned label)
{
    an_gas_print(gas, ".Larchon_%zu_%u:\n", gas->function, label);
}

static unsigned align_up(unsigned size, unsigned align)
{
    return (size + align - 1u) & ~(align - 1u);
}

static int make_frame(ANContext *ctx, const ArchonMIRFunction *fn, GasFrame *frame)
{
    const unsigned word = an_gas_abi.stack_slot_bytes;
    const unsigned align = an_abi.stack_align;
    const unsigned entry = an_gas_abi.stack_entry_remainder;
    unsigned outgoing = 0, phis = 0, size;
    size_t b, i;
    int calls = 0;
    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        if (block->param_count > phis)
            phis = (unsigned)block->param_count;
        for (i = 0; i < block->inst_count; ++i) {
            const ArchonMIRInst *ins = &block->insts[i];
            if (an_is_call(ins->op)) {
                unsigned argc = (unsigned)ins->arg_count;
                if (argc > an_gas_abi.argc && argc - an_gas_abi.argc > outgoing)
                    outgoing = argc - an_gas_abi.argc;
                calls = 1;
            }
        }
    }
    frame->homes = outgoing * word;
    frame->copies = frame->homes + (unsigned)fn->value_count * word;
    size = frame->copies + phis * word;
    if (size > FRAME_LIMIT)
        goto too_large;
    for (i = 0; i < fn->local_count; ++i) {
        const ArchonMIRLocal *local = &fn->locals[i];
        size = align_up(size, local->alignment);
        if (local->size > FRAME_LIMIT - size)
            goto too_large;
        frame->locals[i] = size;
        size += local->size;
    }
    frame->record = -1;
    if (calls && an_gas_abi.link) {
        /* AAPCS frame record: previous FP, then saved LR. */
        size = align_up(size, align);
        frame->record = (int)size;
        size += 2 * word;
    }
    /* Downward adjustment leaves SP aligned at every call. On AMD64 the
     * incoming return PC already accounts for entry % 16 == 8. */
    frame->size = size + ((entry - size) & (align - 1u));
    if (frame->size <= FRAME_LIMIT)
        return 1;
too_large:
    return an_error(ctx, ARCHON_BACKEND_LIMIT, "scalar GAS stack frame exceeds 64 KiB");
}

static unsigned home(const GasFrame *frame, ArchonValue v)
{
    return frame->homes + v * an_gas_abi.stack_slot_bytes;
}

static void parameters(ANGas *gas, const GasFrame *frame, size_t count)
{
    const ANGasABI *abi = &an_gas_abi;
    size_t i;
    for (i = 0; i < count && i < abi->argc; ++i)
        an_gas_stack_store(gas, abi->args[i], home(frame, (ArchonValue)i));
    for (; i < count; ++i) {
        unsigned src = frame->size + abi->stack_arg_offset +
            ((unsigned)i - abi->argc) * abi->stack_slot_bytes;
        an_gas_stack_load(gas, an_gas_a, src);
        an_gas_stack_store(gas, an_gas_a, home(frame, (ArchonValue)i));
    }
}

static void call(ANGas *gas, const GasFrame *frame, const ArchonMIRInst *ins)
{
    const ANGasABI *abi = &an_gas_abi;
    size_t i;
    /* Overflow arguments start at pre-call SP on both profiles. Materialize
     * them before touching the register arguments; all sources are homes. */
    for (i = abi->argc; i < ins->arg_count; ++i) {
        an_gas_stack_load(gas, an_gas_a, home(frame, ins->args[i]));
        an_gas_stack_store(gas, an_gas_a, ((unsigned)i - abi->argc) * abi->stack_slot_bytes);
    }
    for (i = 0; i < ins->arg_count && i < abi->argc; ++i)
        an_gas_stack_load(gas, abi->args[i], home(frame, ins->args[i]));
    if (ins->op == ARCHON_MIR_CALL_INDIRECT) {
        an_gas_stack_load(gas, an_gas_call_target, home(frame, ins->a));
        an_gas_call_indirect(gas);
    } else {
        an_gas_call(gas, ins->symbol);
    }
    if (ins->dst != ARCHON_MIR_NONE)
        an_gas_stack_store(gas, abi->result, home(frame, ins->dst));
}

static int instruction(ANContext *ctx, ANGas *gas, const GasFrame *frame,
    const ArchonMIRInst *ins)
{
    unsigned shape = an_shape(ins->op);
    int disp;
    if (an_is_call(ins->op)) {
        call(gas, frame, ins);
        return 1;
    }
    if (shape & 2u)
        an_gas_stack_load(gas, an_gas_a, home(frame, ins->a));
    if (shape & 4u)
        an_gas_stack_load(gas, an_gas_b, home(frame, ins->b));
    switch (ins->op) {
    case ARCHON_MIR_CONST:
        an_gas_constant(gas, ins->imm);
        break;
    case ARCHON_MIR_COPY:
    case ARCHON_MIR_PTR_TO_INT:
    case ARCHON_MIR_INT_TO_PTR:
    case ARCHON_MIR_FUNC_PTR_TO_INT:
    case ARCHON_MIR_INT_TO_FUNC_PTR:
        break;
    case ARCHON_MIR_TRUNC:
    case ARCHON_MIR_ZEXT:
    case ARCHON_MIR_SEXT:
        an_gas_convert(gas, ins->op, (unsigned)ins->imm);
        break;
    case ARCHON_MIR_LOAD:
    case ARCHON_MIR_LOAD_S8:
    case ARCHON_MIR_LOAD_U8:
    case ARCHON_MIR_LOAD_S16:
    case ARCHON_MIR_LOAD_U16:
    case ARCHON_MIR_LOAD_S32:
    case ARCHON_MIR_LOAD_U32:
    case ARCHON_MIR_LOAD_PTR:
    case ARCHON_MIR_LOAD_FUNC_PTR:
    case ARCHON_MIR_STORE:
    case ARCHON_MIR_STORE8:
    case ARCHON_MIR_STORE16:
    case ARCHON_MIR_STORE32:
    case ARCHON_MIR_STORE_PTR:
    case ARCHON_MIR_STORE_FUNC_PTR:
        /* Signed 12-bit, verified. Avoid an out-of-range unsigned-to-signed cast. */
        disp = ins->imm <= 2047u ? (int)ins->imm : -1 - (int)(UINT64_MAX - ins->imm);
        an_gas_memory(gas, ins->op, disp);
        break;
    case ARCHON_MIR_FRAME_ADDR:
        an_gas_local_addr(gas, frame->locals[ins->imm]);
        break;
    case ARCHON_MIR_GLOBAL_ADDR:
    case ARCHON_MIR_FUNC_ADDR:
        an_gas_symbol_addr(gas, ins->symbol);
        break;
    default:
        if (!an_gas_binary(gas, ins->op))
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "no scalar GAS selection rule");
        break;
    }
    if (shape & 1u)
        an_gas_stack_store(gas, an_gas_a, home(frame, ins->dst));
    return 1;
}

static void edge(ANGas *gas, const ArchonMIRFunction *fn, const GasFrame *frame,
    const ArchonMIREdge *e)
{
    const ArchonMIRBlock *dst = &fn->blocks[e->target];
    unsigned i, word = an_gas_abi.stack_slot_bytes;
    for (i = 0; i < e->arg_count; ++i) {
        an_gas_stack_load(gas, an_gas_a, home(frame, e->args[i]));
        an_gas_stack_store(gas, an_gas_a, frame->copies + i * word);
    }
    for (i = 0; i < e->arg_count; ++i) {
        an_gas_stack_load(gas, an_gas_a, frame->copies + i * word);
        an_gas_stack_store(gas, an_gas_a, home(frame, dst->params[i]));
    }
    an_gas_jump(gas, e->target);
}

static void terminator(ANGas *gas, const ArchonMIRFunction *fn,
    const GasFrame *frame, const ArchonMIRTerm *term)
{
    unsigned yes;
    switch (term->kind) {
    case ARCHON_MIR_RETURN:
        if (term->value != ARCHON_MIR_NONE)
            an_gas_stack_load(gas, an_gas_abi.result, home(frame, term->value));
        an_gas_leave(gas, frame->size, frame->record);
        break;
    case ARCHON_MIR_BR:
        edge(gas, fn, frame, &term->yes);
        break;
    case ARCHON_MIR_CBR:
        yes = gas->next_label++;
        an_gas_stack_load(gas, an_gas_a, home(frame, term->value));
        an_gas_branch_nonzero(gas, yes);
        /* The conditional jump skips only the no-edge copy sequence.
         * At most AN_MAX_VALUE copies: well within A64 CBNZ range, even
         * when stack offsets need materialization. */
        edge(gas, fn, frame, &term->no);
        an_gas_label(gas, yes);
        edge(gas, fn, frame, &term->yes);
        break;
    case ARCHON_MIR_TRAP:
        an_gas_trap(gas);
        break;
    }
}

static int function(ANContext *ctx, ANGas *gas, const ArchonMIRFunction *fn)
{
    GasFrame frame;
    size_t b, i;
    ctx->stage = ARCHON_BACKEND_ALLOCATE;
    if (!make_frame(ctx, fn, &frame))
        return 0;
    gas->next_label = (unsigned)fn->block_count;
    an_gas_print(gas, "\t.p2align 4\n\t.globl %s\n\t.type %s, %%function\n%s:\n",
        fn->name, fn->name, fn->name);
    an_gas_enter(gas, frame.size, frame.record);
    parameters(gas, &frame, fn->param_count);
    ctx->stage = ARCHON_BACKEND_SELECT;
    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *bb = &fn->blocks[b];
        ctx->block = b;
        ctx->instruction = SIZE_MAX;
        if (gas->out->status || !an_continue(ctx))
            return 0;
        an_gas_label(gas, (unsigned)b);
        for (i = 0; i < bb->inst_count; ++i) {
            ctx->instruction = i;
            if (!an_continue(ctx) || !instruction(ctx, gas, &frame, &bb->insts[i]) || gas->out->status)
                return 0;
        }
        ctx->instruction = bb->inst_count;
        terminator(gas, fn, &frame, &bb->term);
    }
    an_gas_print(gas, "\t.size %s, .-%s\n", fn->name, fn->name);
    return gas->out->status == ARCHON_BACKEND_OK;
}

int an_compile_module(ANContext *ctx, const ArchonMIRModule *module, ANOut *out)
{
    ANGas gas = { .out = out };
    size_t i, j;
#ifdef AN_SCALAR_GAS_PIC
    if (ctx->flags & ~(unsigned)ARCHON_BACKEND_PIC)
        return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED,
            "scalar GAS provider supports only assembly in input order (default or PIC)");
#else
    if (ctx->flags)
        return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED,
            "scalar GAS provider supports only non-PIC assembly in input order");
#endif
    an_gas_print(&gas, "\t.text\n");
    for (i = 0; i < module->function_count; ++i) {
        ctx->function = gas.function = i;
        ctx->block = ctx->instruction = SIZE_MAX;
        if (!an_continue(ctx) || !function(ctx, &gas, &module->functions[i]))
            return 0;
    }
    ctx->stage = ARCHON_BACKEND_EMIT;
    ctx->function = ctx->block = ctx->instruction = SIZE_MAX;
    for (i = 0; i < module->global_count; ++i) {
        const ArchonMIRGlobal *g = &module->globals[i];
        if (!g->word_count)
            continue;
        if (!an_continue(ctx))
            return 0;
        an_gas_print(&gas, "\t.%s\n\t.p2align 3\n\t.globl %s\n\t.type %s, %%object\n%s:\n",
            g->words ? "data" : "bss", g->name, g->name, g->name);
        if (g->words) {
            for (j = 0; j < g->word_count; ++j) {
                if (out->status || !an_continue(ctx))
                    return 0;
                an_gas_print(&gas, "\t.quad 0x%" PRIx64 "\n", g->words[j]);
            }
        } else {
            an_gas_print(&gas, "\t.zero %zu\n", g->word_count * 8u);
        }
        an_gas_print(&gas, "\t.size %s, .-%s\n", g->name, g->name);
    }
    an_gas_print(&gas, "\t.section .note.GNU-stack,\"\",%%progbits\n");
    return out->status == ARCHON_BACKEND_OK;
}
