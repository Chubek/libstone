#include "NativeInternal.h"
#include "TargetNative.h"

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

#ifdef AN_RV64_SOFT_FP_TRANSPORT
static void lower_fcompare(ANOut *out, ArchonMIROpcode op, ArchonMIRType type,
    size_t fn, unsigned *next_label)
{
    const unsigned no = (*next_label)++;
    const unsigned done = (*next_label)++;
    const unsigned equal = op == ARCHON_MIR_FLT ? no : (*next_label)++;
    /* LP64 gives no meaning to the upper 32 bits of an incoming f32.
     * Align its sign with f64's and discard those bits before comparing. */
    if (type == ARCHON_MIR_F32) {
        an_emit(out, AN_SLLI, AN_T0, AN_T0, 0, 32);
        an_emit(out, AN_SLLI, AN_T1, AN_T1, 0, 32);
    }

    /* One left shift removes the sign: magnitudes above infinity are NaNs.
     * MIR comparisons are ordered, so an unordered pair returns false. */
    an_li(out, AN_T2, type == ARCHON_MIR_F32 ? UINT64_C(0xff00000000000000) :
                                             UINT64_C(0xffe0000000000000));
    an_emit(out, AN_SLLI, AN_T3, AN_T0, 0, 1);
    an_emit(out, AN_SLTU, AN_T3, AN_T2, AN_T3, 0);
    an_branch(out, 1, AN_T3, AN_ZERO, fn, no);
    an_emit(out, AN_SLLI, AN_T3, AN_T1, 0, 1);
    an_emit(out, AN_SLTU, AN_T3, AN_T2, AN_T3, 0);
    an_branch(out, 1, AN_T3, AN_ZERO, fn, no);

    an_branch(out, 0, AN_T0, AN_T1, fn, equal);
    an_emit(out, AN_OR, AN_T3, AN_T0, AN_T1, 0);
    an_emit(out, AN_SLLI, AN_T3, AN_T3, 0, 1);
    an_branch(out, 0, AN_T3, AN_ZERO, fn, equal); /* Opposite signed zeros. */
    if (op != ARCHON_MIR_FEQ) {
        /* Unequal, ordered operands: unsigned bit ordering is backwards
         * when either operand has its sign bit set. */
        an_emit(out, AN_OR, AN_T3, AN_T0, AN_T1, 0);
        an_emit(out, AN_SLT, AN_T3, AN_T3, AN_ZERO, 0);
        an_emit(out, AN_SLTU, AN_T2, AN_T0, AN_T1, 0);
        an_emit(out, AN_XOR, AN_T2, AN_T2, AN_T3, 0);
        an_jump(out, fn, done);
    }
    an_label(out, fn, no);
    an_emit(out, AN_ADDI, AN_T2, AN_ZERO, 0, 0);
    if (equal != no) {
        an_jump(out, fn, done);
        an_label(out, fn, equal);
        an_emit(out, AN_ADDI, AN_T2, AN_ZERO, 0, 1);
    }
    an_label(out, fn, done);
}
#endif

/* The verifier establishes the generic SSA contract. Lowering visits blocks
 * in layout order, using the same instruction order that allocation checked.
 * Reserved scratch registers are never assigned as value homes. */
static int lower_inst(ANContext *ctx, ANOut *out, const ArchonMIRFunction *fn, const ANFrame *frame,
    const ANHome *homes, const ArchonMIRInst *ins, unsigned *next_label)
{
    unsigned shape = an_shape(ins->op);
    ANOpcode memory;
    int disp;
#ifndef AN_RV64_SOFT_FP_TRANSPORT
    (void)fn;
#endif
    switch (ins->op) {
#ifdef AN_RV64_SOFT_FP_TRANSPORT
    case ARCHON_MIR_F128_PACK:
        an_read(out, frame, homes, ins->b, AN_T1);
        an_store(out, AN_T1, an_frame_base(frame), frame->high_offsets[ins->dst]);
        /* fall through */
    case ARCHON_MIR_F128_LOW:
        an_read(out, frame, homes, ins->a, AN_T2);
        break;
    case ARCHON_MIR_F128_HIGH:
        an_load(out, AN_T2, an_frame_base(frame), frame->high_offsets[ins->a]);
        break;
    case ARCHON_MIR_FCONST:
#endif
    case ARCHON_MIR_CONST:
        an_li(out, AN_T2, ins->imm);
        break;
#ifdef AN_RV64_SOFT_FP_TRANSPORT
    case ARCHON_MIR_FNEG:
        an_read(out, frame, homes, ins->a, AN_T0);
        an_emit(out, AN_ADDI, AN_T1, AN_ZERO, 0, 1);
        an_emit(out, AN_SLLI, AN_T1, AN_T1, 0,
                fn->types[ins->dst] == ARCHON_MIR_F32 ? 31 : 63);
        an_emit(out, AN_XOR, AN_T2, AN_T0, AN_T1, 0);
        break;
    case ARCHON_MIR_FEQ:
    case ARCHON_MIR_FLT:
    case ARCHON_MIR_FLE:
        an_read(out, frame, homes, ins->a, AN_T0);
        an_read(out, frame, homes, ins->b, AN_T1);
        lower_fcompare(out, ins->op, fn->types[ins->a], ctx->function, next_label);
        break;
#endif
    case ARCHON_MIR_COPY:
        if (fn->types[ins->a] == ARCHON_MIR_F128) {
            an_load(out, AN_T1, an_frame_base(frame), frame->high_offsets[ins->a]);
            an_store(out, AN_T1, an_frame_base(frame), frame->high_offsets[ins->dst]);
        }
        /* fall through */
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
#ifdef AN_RV64_SOFT_FP_TRANSPORT
    case ARCHON_MIR_FLOAD:
    case ARCHON_MIR_FSTORE:
#endif
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
#ifdef AN_RV64_SOFT_FP_TRANSPORT
        if (ins->op == ARCHON_MIR_FLOAD)
            memory = fn->types[ins->dst] == ARCHON_MIR_F32 ? AN_LWU : AN_LD;
        else
#endif
            memory = load_opcode(ins->op);
        if (memory == AN_OPCODE_COUNT) {
#ifdef AN_RV64_SOFT_FP_TRANSPORT
            if (ins->op == ARCHON_MIR_FSTORE)
                memory = fn->types[ins->b] == ARCHON_MIR_F32 ? AN_SW : AN_SD;
            else
#endif
                memory = store_opcode(ins->op);
            an_read(out, frame, homes, ins->b, AN_T1);
            an_emit(out, memory, AN_T1, AN_T0, 0, disp);
        } else {
            an_emit(out, memory, AN_T2, AN_T0, 0, disp);
        }
        break;
    case ARCHON_MIR_FRAME_ADDR:
        an_addr(out, AN_T2, an_frame_base(frame), frame->local_offsets[ins->imm]);
        break;
    case ARCHON_MIR_STACK_ALLOC: {
        const unsigned loop = (*next_label)++, tail = (*next_label)++;
        const unsigned invalid = (*next_label)++, done = (*next_label)++;
        const unsigned sp = an_abi.stack_pointer;
        const size_t f = ctx->function;
        an_read(out, frame, homes, ins->a, AN_T0);
        an_branch(out, 0, AN_T0, AN_ZERO, f, invalid);
        an_emit(out, AN_ADDI, AN_T1, AN_T0, 0, 15);
        an_emit(out, AN_SLTU, AN_T2, AN_T1, AN_T0, 0);
        an_branch(out, 1, AN_T2, AN_ZERO, f, invalid);
        an_li(out, AN_T0, UINT64_MAX - 15);
        an_emit(out, AN_AND, AN_T0, AN_T1, AN_T0, 0);
        an_li(out, AN_T1, 4096);
        an_label(out, f, loop);
        an_emit(out, AN_SLTU, AN_T2, AN_T1, AN_T0, 0);
        an_branch(out, 0, AN_T2, AN_ZERO, f, tail);
        an_emit(out, AN_SUB, sp, sp, AN_T1, 0);
        an_store(out, AN_ZERO, sp, 0);
        an_emit(out, AN_SUB, AN_T0, AN_T0, AN_T1, 0);
        an_jump(out, f, loop);
        an_label(out, f, tail);
        an_emit(out, AN_SUB, sp, sp, AN_T0, 0);
        an_store(out, AN_ZERO, sp, 0);
        an_addr(out, AN_T2, sp, frame->slots_base);
        an_jump(out, f, done);
        an_label(out, f, invalid);
        an_emit(out, AN_EBREAK, 0, 0, 0, 0);
        an_label(out, f, done);
        break;
    }
    case ARCHON_MIR_STACK_FREE: {
        const unsigned invalid = (*next_label)++, done = (*next_label)++;
        const unsigned sp = an_abi.stack_pointer;
        const size_t f = ctx->function;
        an_read(out, frame, homes, ins->a, AN_T0);
        an_emit(out, AN_ADDI, AN_T1, AN_T0, 0, 15);
        an_emit(out, AN_SLTU, AN_T2, AN_T1, AN_T0, 0);
        an_branch(out, 1, AN_T2, AN_ZERO, f, invalid);
        an_li(out, AN_T0, UINT64_MAX - 15);
        an_emit(out, AN_AND, AN_T1, AN_T1, AN_T0, 0);
        an_emit(out, AN_ADD, AN_T1, sp, AN_T1, 0);
        an_emit(out, AN_SLTU, AN_T2, AN_T1, sp, 0);
        an_branch(out, 1, AN_T2, AN_ZERO, f, invalid);
        an_emit(out, AN_SLTU, AN_T2, an_frame_base(frame), AN_T1, 0);
        an_branch(out, 1, AN_T2, AN_ZERO, f, invalid);
        an_addr(out, sp, AN_T1, 0);
        an_jump(out, f, done);
        an_label(out, f, invalid);
        an_emit(out, AN_EBREAK, 0, 0, 0, 0);
        an_label(out, f, done);
        break;
    }
    case ARCHON_MIR_RV64_VA_START:
        an_read(out, frame, homes, ins->a, AN_T0);
        an_addr(out, AN_T2, an_frame_base(frame), frame->va_start_offset);
        an_store(out, AN_T2, AN_T0, 0);
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
        if (an_fact_conversion_is_copy(ctx, ins)) {
            an_read(out, frame, homes, ins->a, AN_T2);
        } else {
            an_read(out, frame, homes, ins->a, AN_T0);
            an_convert(out, ins->op, AN_T2, AN_T0, AN_T1, (unsigned)ins->imm);
        }
        break;
    default: {
        unsigned shift;
        ArchonValue source = an_fact_mul_shift(ctx, ins, &shift);
        if (source != ARCHON_MIR_NONE) {
            an_read(out, frame, homes, source, AN_T0);
            an_select_shift_left_imm(out, AN_T2, AN_T0, shift);
            break;
        }
        if (shape & 2u)
            an_read(out, frame, homes, ins->a, AN_T0);
        if (shape & 4u)
            an_read(out, frame, homes, ins->b, AN_T1);
        if (!an_select(out, ins->op, AN_T2, AN_T0, AN_T1, ctx->function, next_label))
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "no native selection rule");
    }
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
    int calls = 0, variadic = 0, ok = 0;
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
        const size_t words = an_edge_words(fn->types, block->params, block->param_count);
        if (words > phi_count)
            phi_count = words;
        for (i = 0; i < block->inst_count; ++i) {
            const ArchonMIRInst *ins = &block->insts[i];
            variadic |= ins->op == ARCHON_MIR_RV64_VA_START;
            if (an_is_call(ins->op)) {
                size_t outgoing = an_outgoing_words(an_argument_slots(fn->types, ins->args, ins->arg_count));
                if (outgoing > outgoing_words)
                    outgoing_words = outgoing;
                calls = 1;
            }
        }
    }
    if (!an_frame(ctx, fn, slots, phi_count, outgoing_words, calls, variadic, &frame) ||
        !an_verify_frame(ctx, fn, slots, phi_count, outgoing_words, calls, &frame))
        goto done;
    ctx->stage = ARCHON_BACKEND_SELECT;
    if (!an_layout_begin(out, ctx->function, fn->name)) {
        (void)emission_ok(ctx, out);
        goto done;
    }
    an_asm_function(out, fn->name);
    an_enter(out, &frame);
    if (variadic) {
        /* Save incoming unnamed register slots once, before the body can
         * clobber them. In RV64IM LP64 even floating scalars use XLEN slots. */
        const size_t named = an_argument_slots(fn->types, NULL, fn->param_count);
        for (i = named; i < an_abi.argc; ++i)
            an_store(out, an_abi.args[i], an_frame_base(&frame),
                frame.va_start_offset + (int)(8 * (i - named)));
    }
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
                !lower_inst(ctx, out, fn, &frame, homes, &block->insts[i], &next_label) ||
                !emission_ok(ctx, out))
                goto done;
        }
        ctx->instruction = block->inst_count;
        an_terminator(ctx, out, fn, &frame, homes, &block->term, &next_label);
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
        ANProvenBits *proved;
        int ok;
        ctx->function = f;
        ctx->block = ctx->instruction = SIZE_MAX;
        if (!an_continue(ctx))
            return 0;
        proved = an_prove_known_bits(ctx, &module->functions[f]);
        ctx->proven_bits = proved;
        ok = compile_function(ctx, &module->functions[f], out);
        ctx->proven_bits = NULL;
        free(proved);
        if (!ok)
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
