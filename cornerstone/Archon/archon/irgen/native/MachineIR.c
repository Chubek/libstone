#include "NativeInternal.h"

#include <stdlib.h>

static ANOpcode load_opcode(ArchonMIROpcode op)
{
    switch (op) {
    case ARCHON_MIR_LOAD_S8: return AN_LB;
    case ARCHON_MIR_LOAD_U8: return AN_LBU;
    case ARCHON_MIR_LOAD_S16: return AN_LH;
    case ARCHON_MIR_LOAD_U16: return AN_LHU;
    case ARCHON_MIR_LOAD_S32: return AN_LW;
    case ARCHON_MIR_LOAD_U32: return AN_LWU;
    case ARCHON_MIR_LOAD:
    case ARCHON_MIR_LOAD_PTR:
    case ARCHON_MIR_LOAD_FUNC_PTR:
        return AN_LD;
    default:
        return AN_OPCODE_COUNT;
    }
}

static ANOpcode store_opcode(ArchonMIROpcode op)
{
    switch (op) {
    case ARCHON_MIR_STORE8: return AN_SB;
    case ARCHON_MIR_STORE16: return AN_SH;
    case ARCHON_MIR_STORE32: return AN_SW;
    case ARCHON_MIR_STORE:
    case ARCHON_MIR_STORE_PTR:
    case ARCHON_MIR_STORE_FUNC_PTR:
        return AN_SD;
    default:
        return AN_OPCODE_COUNT;
    }
}

/* The verifier establishes the generic SSA contract. Lowering visits blocks
 * in layout order, using the same instruction order that allocation checked.
 * Reserved scratch registers are never assigned as value homes. */
static int lower_inst(ANContext *ctx, ANOut *out, const ANFrame *frame,
    const ANHome *homes, const ArchonMIRInst *ins, unsigned *next_label)
{
    unsigned shape = an_shape(ins->op);
    ANOpcode memory;
    int disp;
    switch (ins->op) {
    case ARCHON_MIR_CONST:
        an_li(out, AN_T2, ins->imm);
        break;
    case ARCHON_MIR_COPY:
    case ARCHON_MIR_PTR_TO_INT:
    case ARCHON_MIR_INT_TO_PTR:
    case ARCHON_MIR_FUNC_PTR_TO_INT:
    case ARCHON_MIR_INT_TO_FUNC_PTR:
        an_read(out, frame, homes, ins->a, AN_T2);
        break;
    case ARCHON_MIR_PTR_ADD:
        an_read(out, frame, homes, ins->a, AN_T0);
        an_read(out, frame, homes, ins->b, AN_T1);
        an_emit(out, AN_ADD, AN_T2, AN_T0, AN_T1, 0);
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
        /* an_legal admits only signed 12-bit, naturally aligned displacements. */
        disp = ins->imm <= INT32_MAX ? (int)ins->imm :
            -1 - (int)(UINT64_MAX - ins->imm);
        an_read(out, frame, homes, ins->a, AN_T0);
        memory = load_opcode(ins->op);
        if (memory == AN_OPCODE_COUNT) {
            memory = store_opcode(ins->op);
            an_read(out, frame, homes, ins->b, AN_T1);
            an_emit(out, memory, AN_T1, AN_T0, 0, disp);
        } else {
            an_emit(out, memory, AN_T2, AN_T0, 0, disp);
        }
        break;
    case ARCHON_MIR_FRAME_ADDR:
        an_addr(out, AN_T2, an_abi.stack_pointer, frame->local_offsets[ins->imm]);
        break;
    case ARCHON_MIR_GLOBAL_ADDR:
    case ARCHON_MIR_FUNC_ADDR:
        an_address_symbol(out, AN_T2, ins->symbol);
        break;
    case ARCHON_MIR_CALL:
        an_call(out, ins, frame, homes);
        return 1; /* ABI lowering handles the result home, including void. */
    case ARCHON_MIR_CALL_INDIRECT:
        an_call_indirect(out, ins, frame, homes);
        return 1;
    case ARCHON_MIR_TRUNC:
    case ARCHON_MIR_ZEXT:
    case ARCHON_MIR_SEXT:
        an_read(out, frame, homes, ins->a, AN_T0);
        an_convert(out, ins->op, AN_T2, AN_T0, AN_T1, (unsigned)ins->imm);
        break;
    default:
        if (shape & 2u)
            an_read(out, frame, homes, ins->a, AN_T0);
        if (shape & 4u)
            an_read(out, frame, homes, ins->b, AN_T1);
        if (!an_select(out, ins->op, AN_T2, AN_T0, AN_T1, ctx->function, next_label))
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "no native selection rule");
    }
    if (shape & 1u)
        an_write(out, frame, homes, ins->dst, AN_T2);
    return 1;
}

static int emission_ok(ANContext *ctx, const ANOut *out)
{
    if (out->status == ARCHON_BACKEND_OK)
        return 1;
    ctx->stage = ARCHON_BACKEND_EMIT;
    return an_error(ctx, out->status,
        out->detail ? out->detail : out->status == ARCHON_BACKEND_LIMIT ? "backend output limit exceeded" :
        out->status == ARCHON_BACKEND_NOMEM ? "allocating backend output" :
        "physical emission rejected an instruction or operand");
}

static int schedule_function(ANContext *ctx, const ArchonMIRFunction *fn,
    ArchonMIRBlock *blocks, ArchonMIRInst **storage)
{
    size_t b, offset = 0, count = 0;
    ctx->stage = ARCHON_BACKEND_SCHEDULE;
    for (b = 0; b < fn->block_count; ++b)
        count += fn->blocks[b].inst_count; /* Verified aggregate function limit. */
    if (count) {
        *storage = malloc(count * sizeof(**storage));
        if (!*storage)
            return an_error(ctx, ARCHON_BACKEND_NOMEM, "allocating scheduled instructions");
    }
    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        blocks[b] = *block;
        ctx->block = b;
        ctx->instruction = SIZE_MAX;
        if (!an_continue(ctx))
            return 0;
        if (block->inst_count) {
            ArchonMIRInst *next = *storage + offset;
            if (!an_schedule_block(ctx, block, next))
                return 0;
            blocks[b].insts = next;
        }
        offset += block->inst_count;
    }
    ctx->block = ctx->instruction = SIZE_MAX;
    return 1;
}

static int compile_function(ANContext *ctx, const ArchonMIRFunction *source, ANOut *out)
{
    const ArchonMIRFunction *fn = source;
    ArchonMIRFunction scheduled;
    ArchonMIRBlock *blocks = NULL;
    ArchonMIRInst *storage = NULL;
    size_t slots = 0, phi_count = 0, outgoing_words = 0, b, i;
    unsigned next_label = (unsigned)fn->block_count;
    int calls = 0, ok = 0;
    ANFrame frame;
    ANHome *homes = NULL;
    if (ctx->flags & ARCHON_BACKEND_SCHEDULE_PURE) {
        blocks = calloc(source->block_count, sizeof(*blocks));
        if (!blocks)
            return an_error(ctx, ARCHON_BACKEND_NOMEM, "allocating scheduled blocks");
        if (!schedule_function(ctx, source, blocks, &storage))
            goto done;
        scheduled = *source;
        scheduled.blocks = blocks;
        fn = &scheduled;
    }
    ctx->stage = ARCHON_BACKEND_ALLOCATE;
    homes = calloc(fn->value_count ? fn->value_count : 1, sizeof(*homes));
    if (!homes) {
        an_error(ctx, ARCHON_BACKEND_NOMEM, "allocating register homes");
        goto done;
    }
    if (!an_allocate(ctx, fn, homes, &slots) ||
        !an_verify_homes(ctx, fn, homes, slots))
        goto done;
    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        if (block->param_count > phi_count)
            phi_count = block->param_count;
        for (i = 0; i < block->inst_count; ++i) {
            const ArchonMIRInst *ins = &block->insts[i];
            if (an_is_call(ins->op)) {
                size_t words = an_outgoing_words(ins->arg_count);
                if (words > outgoing_words)
                    outgoing_words = words;
                calls = 1;
            }
        }
    }
    if (!an_frame(ctx, fn, slots, phi_count, outgoing_words, calls, &frame) ||
        !an_verify_frame(ctx, fn, slots, phi_count, outgoing_words, calls, &frame))
        goto done;
    ctx->stage = ARCHON_BACKEND_SELECT;
    if (!an_layout_begin(out, ctx->function, fn->name)) {
        (void)emission_ok(ctx, out);
        goto done;
    }
    an_asm_function(out, fn->name);
    an_enter(out, &frame);
    an_parameters(out, fn, &frame, homes);
    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        ctx->block = b;
        ctx->instruction = SIZE_MAX;
        if (!emission_ok(ctx, out) || !an_continue(ctx))
            goto done;
        an_label(out, ctx->function, (unsigned)b);
        if (!emission_ok(ctx, out))
            goto done;
        for (i = 0; i < block->inst_count; ++i) {
            ctx->instruction = i;
            if (!an_continue(ctx) ||
                !lower_inst(ctx, out, &frame, homes, &block->insts[i], &next_label) ||
                !emission_ok(ctx, out))
                goto done;
        }
        ctx->instruction = block->inst_count;
        an_terminator(out, ctx->function, fn, &frame, homes, &block->term, &next_label);
        if (!emission_ok(ctx, out))
            goto done;
    }
    if (!an_layout_finish(ctx, out, next_label)) {
        (void)emission_ok(ctx, out);
        goto done;
    }
    an_asm_end_function(out, fn->name);
    ok = emission_ok(ctx, out);
done:
    an_layout_dispose(out);
    free(homes);
    free(blocks);
    free(storage);
    return ok;
}

int an_compile_module(ANContext *ctx, const ArchonMIRModule *module, ANOut *out)
{
    size_t f;
    ctx->stage = ARCHON_BACKEND_EMIT;
    ctx->function = ctx->block = ctx->instruction = SIZE_MAX;
    if (!an_continue(ctx))
        return 0;
    if (out->machine_code && (module->function_count != 1 || module->import_count ||
            module->global_count))
        return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED,
            "raw machine code requires exactly one function, no imports or globals");
    an_asm_header(out);
    if (!emission_ok(ctx, out))
        return 0;
    for (f = 0; f < module->function_count; ++f) {
        ctx->function = f;
        ctx->block = ctx->instruction = SIZE_MAX;
        if (!an_continue(ctx) || !compile_function(ctx, &module->functions[f], out))
            return 0;
    }
    ctx->stage = ARCHON_BACKEND_EMIT;
    ctx->function = ctx->block = ctx->instruction = SIZE_MAX;
    if (!an_continue(ctx))
        return 0;
    if (!an_asm_globals(ctx, out, module))
        return 0;
    an_asm_footer(out);
    return emission_ok(ctx, out) && an_continue(ctx);
}
