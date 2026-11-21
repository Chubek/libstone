#include "ScalarGAS.h"
#include "TargetNative.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_LIMIT 65536u

typedef enum { GAS_GP, GAS_FP, GAS_STACK } GasArgKind;
typedef struct { GasArgKind kind; unsigned index; } GasArg;
typedef struct { unsigned gp, fp, stack; } GasArgState;

/* All MIR values occupy a word, and every region is disjoint. Offsets are
 * nonnegative from the adjusted stack pointer, which remains fixed throughout
 * the body (including during call setup). */
typedef struct {
    unsigned size, homes, copies, locals[AN_MAX_VALUE];
    int record;
#if defined(AN_SYSV_AMD64) || defined(AN_AAPCS64)
    int varargs;
    GasArgState named;
#endif
} GasFrame;

/* Both reviewed profiles allocate integer/pointer and FP arguments from
 * independent register banks. Overflow scalars share one stack area in
 * declaration order, with 8-byte slots for these types. */
static GasArg argument(GasArgState *state, ArchonMIRType type)
{
    if (an_is_float(type)) {
        if (state->fp < an_gas_abi.fp_argc)
            return (GasArg){ GAS_FP, state->fp++ };
    } else if (state->gp < an_gas_abi.argc) {
        return (GasArg){ GAS_GP, state->gp++ };
    }
    return (GasArg){ GAS_STACK, state->stack++ };
}

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
#if defined(AN_SYSV_AMD64) || defined(AN_AAPCS64)
    int variadic = 0;
#endif
    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        if (block->param_count > phis)
            phis = (unsigned)block->param_count;
        for (i = 0; i < block->inst_count; ++i) {
            const ArchonMIRInst *ins = &block->insts[i];
#ifdef AN_SYSV_AMD64
            variadic |= ins->op == ARCHON_MIR_AMD64_VA_START;
#elif defined(AN_AAPCS64)
            variadic |= ins->op == ARCHON_MIR_AARCH64_VA_START;
#endif
            if (an_is_call(ins->op)) {
                GasArgState state = {0};
                size_t n;
                for (n = 0; n < ins->arg_count; ++n)
                    (void)argument(&state, fn->types[ins->args[n]]);
                if (state.stack > outgoing)
                    outgoing = state.stack;
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
#if defined(AN_SYSV_AMD64) || defined(AN_AAPCS64)
    frame->varargs = -1;
    if (variadic) {
        frame->named = (GasArgState){0};
        for (i = 0; i < fn->param_count; ++i)
            (void)argument(&frame->named, fn->types[i]);
        size = align_up(size, 16);
        frame->varargs = (int)size;
#ifdef AN_SYSV_AMD64
        size += AN_AMD64_VA_SAVE_BYTES;
#else
        size += AN_AARCH64_VA_SAVE_BYTES;
#endif
    }
#endif
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

static void parameters(ANGas *gas, const GasFrame *frame, const ArchonMIRFunction *fn)
{
    const ANGasABI *abi = &an_gas_abi;
    GasArgState state = {0};
    size_t i;
    for (i = 0; i < fn->param_count; ++i) {
        ArchonMIRType type = fn->types[i];
        GasArg loc = argument(&state, type);
        unsigned dst = home(frame, (ArchonValue)i);
        switch (loc.kind) {
        case GAS_GP:
            an_gas_stack_store(gas, abi->args[loc.index], dst);
            break;
        case GAS_FP:
            an_gas_fp_store(gas, type, loc.index, dst);
            break;
        case GAS_STACK:
            an_gas_stack_load(gas, an_gas_a,
                frame->size + abi->stack_arg_offset + loc.index * abi->stack_slot_bytes);
            if (type == ARCHON_MIR_F32)
                an_gas_convert(gas, ARCHON_MIR_TRUNC, 32);
            an_gas_stack_store(gas, an_gas_a, dst);
            break;
        }
    }
}

static void call(ANGas *gas, const GasFrame *frame, const ArchonMIRFunction *fn,
    const ArchonMIRInst *ins)
{
    const ANGasABI *abi = &an_gas_abi;
    GasArgState state = {0};
    size_t i;
    /* Every source is a home. Neither profile's stack scratch can clobber
     * argument registers, including when loading later overflow arguments. */
    for (i = 0; i < ins->arg_count; ++i) {
        ArchonValue arg = ins->args[i];
        ArchonMIRType type = fn->types[arg];
        GasArg loc = argument(&state, type);
        unsigned src = home(frame, arg);
        switch (loc.kind) {
        case GAS_GP:
            an_gas_stack_load(gas, abi->args[loc.index], src);
            break;
        case GAS_FP:
            an_gas_fp_load(gas, type, loc.index, src);
            break;
        case GAS_STACK:
            an_gas_stack_load(gas, an_gas_a, src);
            an_gas_stack_store(gas, an_gas_a, loc.index * abi->stack_slot_bytes);
            break;
        }
    }
    if (ins->op == ARCHON_MIR_CALL_INDIRECT) {
        an_gas_stack_load(gas, an_gas_call_target, home(frame, ins->a));
        an_gas_call_indirect(gas, state.fp);
    } else {
        an_gas_call(gas, ins->symbol, state.fp);
    }
    if (ins->dst != ARCHON_MIR_NONE) {
        ArchonMIRType type = fn->types[ins->dst];
        if (an_is_float(type))
            an_gas_fp_store(gas, type, 0, home(frame, ins->dst));
        else
            an_gas_stack_store(gas, abi->result, home(frame, ins->dst));
    }
}

/* These integer lowerings leave the result in a, including after its home
 * store. Do not infer this for new opcodes from their MIR shape alone. */
static int forwards_integer(ArchonMIROpcode op)
{
    switch (op) {
    case ARCHON_MIR_COPY:
    case ARCHON_MIR_TRUNC: case ARCHON_MIR_ZEXT: case ARCHON_MIR_SEXT:
    case ARCHON_MIR_ADD: case ARCHON_MIR_SUB: case ARCHON_MIR_MUL:
    case ARCHON_MIR_AND: case ARCHON_MIR_OR: case ARCHON_MIR_XOR:
    case ARCHON_MIR_SHL: case ARCHON_MIR_LSHR: case ARCHON_MIR_ASHR:
    case ARCHON_MIR_EQ: case ARCHON_MIR_SLT: case ARCHON_MIR_ULT:
        return 1;
    default:
        return 0;
    }
}

static int forward_lhs(ANContext *ctx, const ArchonMIRFunction *fn,
    const ArchonMIRInst *prev, const ArchonMIRInst *ins)
{
    /* Independently prove the reload redundant: this is the very next
     * instruction in the same block, its lhs is the exact SSA value just
     * stored, and its home store and a possible rhs load preserve a. The
     * omitted load could also write the dedicated address scratch, which is
     * dead between stack moves. No home store, effect, or order changes. */
    return prev && prev->dst == ins->a &&
        forwards_integer(ins->op) &&
        (prev->op == ARCHON_MIR_CONST || forwards_integer(prev->op)) &&
        fn->types[prev->dst] == ARCHON_MIR_I64 &&
        an_fact(ctx, ARCHON_BACKEND_FACT_OBSERVABLE_INPUT, prev->dst);
}

static int instruction(ANContext *ctx, ANGas *gas, const GasFrame *frame,
    const ArchonMIRFunction *fn, const ArchonMIRInst *ins, const ArchonMIRInst *prev)
{
    ArchonMIROpcode op = ins->op;
    unsigned shape = an_shape(op);
    int disp;
    if (an_is_call(op)) {
        call(gas, frame, fn, ins);
        return 1;
    }
    if (op == ARCHON_MIR_MUL) {
        unsigned shift;
        ArchonValue source = an_fact_mul_shift(ctx, ins, &shift);
        if (source != ARCHON_MIR_NONE) {
            an_gas_stack_load(gas, an_gas_a, home(frame, source));
            an_gas_shift_left_imm(gas, shift);
            an_gas_stack_store(gas, an_gas_a, home(frame, ins->dst));
            return 1;
        }
    }
    if ((shape & 2u) && !forward_lhs(ctx, fn, prev, ins))
        an_gas_stack_load(gas, an_gas_a, home(frame, ins->a));
    if (shape & 4u)
        an_gas_stack_load(gas, an_gas_b, home(frame, ins->b));
    switch (op) {
    case ARCHON_MIR_CONST:
    case ARCHON_MIR_FCONST:
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
        if (!an_fact_conversion_is_copy(ctx, ins))
            an_gas_convert(gas, op, (unsigned)ins->imm);
        break;
    case ARCHON_MIR_FNEG:
    case ARCHON_MIR_FADD:
    case ARCHON_MIR_FSUB:
    case ARCHON_MIR_FMUL:
    case ARCHON_MIR_FDIV:
    case ARCHON_MIR_FEQ:
    case ARCHON_MIR_FLT:
    case ARCHON_MIR_FLE:
    case ARCHON_MIR_FCAST:
    case ARCHON_MIR_SITOFP:
    case ARCHON_MIR_UITOFP:
    case ARCHON_MIR_FPTOSI:
    case ARCHON_MIR_FPTOUI:
        an_gas_float(gas, op, fn->types[ins->a], fn->types[ins->dst]);
        break;
    case ARCHON_MIR_FLOAD:
    case ARCHON_MIR_FSTORE:
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
        /* FP object accesses preserve raw bits and use exactly the width
         * prescribed by the type; the integer move does not reinterpret them. */
        if (op == ARCHON_MIR_FLOAD)
            op = fn->types[ins->dst] == ARCHON_MIR_F32 ? ARCHON_MIR_LOAD_U32 : ARCHON_MIR_LOAD;
        else if (op == ARCHON_MIR_FSTORE)
            op = fn->types[ins->b] == ARCHON_MIR_F32 ? ARCHON_MIR_STORE32 : ARCHON_MIR_STORE;
        an_gas_memory(gas, op, disp);
        break;
    case ARCHON_MIR_FRAME_ADDR:
        an_gas_local_addr(gas, frame->locals[ins->imm]);
        break;
#ifdef AN_SYSV_AMD64
    case ARCHON_MIR_AMD64_X87_ADD:
    case ARCHON_MIR_AMD64_X87_SUB:
    case ARCHON_MIR_AMD64_X87_MUL:
    case ARCHON_MIR_AMD64_X87_DIV:
    case ARCHON_MIR_AMD64_X87_EQ:
    case ARCHON_MIR_AMD64_X87_LT:
    case ARCHON_MIR_AMD64_X87_LE:
    case ARCHON_MIR_AMD64_X87_FROM_FP:
    case ARCHON_MIR_AMD64_X87_TO_FP:
    case ARCHON_MIR_AMD64_X87_FROM_SI:
    case ARCHON_MIR_AMD64_X87_FROM_UI:
    case ARCHON_MIR_AMD64_X87_TO_SI:
    case ARCHON_MIR_AMD64_X87_TO_UI:
        an_amd64_x87(gas, op, (shape & 4u) ? fn->types[ins->b] : ARCHON_MIR_VOID,
                     (shape & 1u) ? fn->types[ins->dst] : ARCHON_MIR_VOID);
        break;
    case ARCHON_MIR_AMD64_VA_START:
        an_amd64_va_start(gas, (unsigned)frame->varargs,
            frame->size + an_gas_abi.stack_arg_offset +
                frame->named.stack * an_gas_abi.stack_slot_bytes,
            frame->named.gp, frame->named.fp);
        break;
#elif defined(AN_AAPCS64)
    case ARCHON_MIR_AARCH64_VA_START:
        an_aarch64_va_start(gas, (unsigned)frame->varargs,
            frame->size + an_gas_abi.stack_arg_offset +
                frame->named.stack * an_gas_abi.stack_slot_bytes,
            frame->named.gp, frame->named.fp);
        break;
#endif
    case ARCHON_MIR_GLOBAL_ADDR:
    case ARCHON_MIR_FUNC_ADDR:
        an_gas_symbol_addr(gas, ins->symbol);
        break;
    default:
        if (!an_gas_binary(gas, op))
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "no scalar GAS selection rule");
        break;
    }
    if (shape & 1u)
        an_gas_stack_store(gas, an_gas_a, home(frame, ins->dst));
    return 1;
}

static void edge(ANGas *gas, const ArchonMIRFunction *fn, const GasFrame *frame,
    const ArchonMIREdge *e, int fallthrough)
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
    if (!fallthrough)
        an_gas_jump(gas, e->target);
}

static void terminator(ANContext *ctx, ANGas *gas, const ArchonMIRFunction *fn,
    const GasFrame *frame, const ArchonMIRTerm *term, const ArchonMIRInst *prev)
{
    unsigned yes;
    /* Independently establish adjacency in actual emission order. The
     * CBR no-edge sequence must jump around the subsequent yes stub. */
    int fallthrough = (term->kind == ARCHON_MIR_BR || term->kind == ARCHON_MIR_CBR) &&
        ctx->block < fn->block_count && ctx->block + 1 < fn->block_count &&
        term->yes.target == ctx->block + 1 &&
        an_fact(ctx, ARCHON_BACKEND_FACT_CONTROL_EDGE, term->yes.target);
    switch (term->kind) {
    case ARCHON_MIR_RETURN:
        if (term->value != ARCHON_MIR_NONE) {
            unsigned offset = home(frame, term->value);
            if (an_is_float(fn->result))
                an_gas_fp_load(gas, fn->result, 0, offset);
            else if (fn->result == ARCHON_MIR_I64 && prev && prev->dst == term->value &&
                     (prev->op == ARCHON_MIR_CONST || forwards_integer(prev->op)) &&
                     an_fact(ctx, ARCHON_BACKEND_FACT_RETURN_VALUE, term->value)) {
                /* The immediately preceding lowering left this exact value
                 * in a, and its home store preserves a. Both scalar epilogues
                 * preserve the ABI result register during frame teardown. */
#ifdef AN_AAPCS64
                an_gas_print(gas, "\tmov %s, %s\n", an_gas_abi.result, an_gas_a);
#else
                if (strcmp(an_gas_a, an_gas_abi.result))
                    an_gas_stack_load(gas, an_gas_abi.result, offset);
#endif
            } else
                an_gas_stack_load(gas, an_gas_abi.result, offset);
        }
        an_gas_leave(gas, frame->size, frame->record);
        break;
    case ARCHON_MIR_BR:
        edge(gas, fn, frame, &term->yes, fallthrough);
        break;
    case ARCHON_MIR_CBR:
        yes = gas->next_label++;
        an_gas_stack_load(gas, an_gas_a, home(frame, term->value));
        an_gas_branch_nonzero(gas, yes);
        /* The conditional jump skips only the no-edge copy sequence.
         * At most AN_MAX_VALUE copies: well within A64 CBNZ range, even
         * when stack offsets need materialization. */
        edge(gas, fn, frame, &term->no, 0);
        an_gas_label(gas, yes);
        edge(gas, fn, frame, &term->yes, fallthrough);
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
#ifdef AN_SYSV_AMD64
    if (frame.varargs >= 0)
        an_amd64_save_varargs(gas, (unsigned)frame.varargs, frame.named.gp, frame.named.fp);
#elif defined(AN_AAPCS64)
    if (frame.varargs >= 0)
        an_aarch64_save_varargs(gas, (unsigned)frame.varargs, frame.named.gp, frame.named.fp);
#endif
    parameters(gas, &frame, fn);
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
            if (!an_continue(ctx) ||
                !instruction(ctx, gas, &frame, fn, &bb->insts[i], i ? &bb->insts[i - 1] : NULL) ||
                gas->out->status)
                return 0;
        }
        ctx->instruction = bb->inst_count;
        terminator(ctx, gas, fn, &frame, &bb->term, i ? &bb->insts[i - 1] : NULL);
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
        ANProvenBits *proved;
        int ok;
        ctx->function = gas.function = i;
        ctx->block = ctx->instruction = SIZE_MAX;
        if (!an_continue(ctx))
            return 0;
        proved = an_prove_known_bits(ctx, &module->functions[i]);
        ctx->proven_bits = proved;
        ok = function(ctx, &gas, &module->functions[i]);
        ctx->proven_bits = NULL;
        free(proved);
        if (!ok)
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
