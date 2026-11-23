#include "NativeInternal.h"

#include <limits.h>

enum { WordBytes = 8 };

/* Check actual half-open intervals in frame order. Track the end of the
 * preceding interval instead of reproducing the layout's padding arithmetic. */
static int region(int offset, uintmax_t bytes, uint32_t alignment,
    uintmax_t size, uintmax_t *end)
{
    uintmax_t start;
    if (offset < 0)
        return 0;
    start = (uintmax_t)offset;
    if (start < *end || start > size || start % alignment || bytes > size - start)
        return 0;
    *end = start + bytes;
    return 1;
}

static int words(int offset, size_t count, uintmax_t size, uintmax_t *end)
{
    /* Check before multiplying, including when a corrupted count is SIZE_MAX. */
    return count <= size / WordBytes &&
        region(offset, (uintmax_t)count * WordBytes, WordBytes, size, end);
}

/* Derive the required capacities from the accesses lowering actually emits:
 * active edge arguments and call operands, not destination blocks or an ABI
 * worst-case allowance. Home indices are checked separately by an_verify_homes. */
static int verify_uses(ANContext *ctx, const ArchonMIRFunction *fn, size_t phi_count,
    size_t outgoing_words, int calls, int variadic, size_t spare_slots)
{
    size_t b, i, needed_phi = 0, needed_outgoing = 0, insts_left = AN_MAX_INSTS;
    int has_call = 0, has_va_start = 0;

    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        const ArchonMIRTerm *term = &block->term;
        size_t edge_words = 0;
        ctx->block = b;
        ctx->instruction = SIZE_MAX;
        if (!an_continue(ctx))
            return 0;
        if (block->inst_count > insts_left || (block->inst_count && !block->insts))
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid frame instruction table");
        insts_left -= block->inst_count;
        if (block->param_count > spare_slots)
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "missing frame slots for block parameters");
        spare_slots -= block->param_count;
        if (term->kind == ARCHON_MIR_BR || term->kind == ARCHON_MIR_CBR)
            edge_words = an_edge_words(fn->types, term->yes.args, term->yes.arg_count);
        if (term->kind == ARCHON_MIR_CBR) {
            size_t no = an_edge_words(fn->types, term->no.args, term->no.arg_count);
            if (no > edge_words)
                edge_words = no;
        }
        if (edge_words > phi_count)
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "insufficient frame phi words for edge");
        if (edge_words > needed_phi)
            needed_phi = edge_words;
        for (i = 0; i < block->inst_count; ++i) {
            const ArchonMIRInst *inst = &block->insts[i];
            size_t call_words;
            ctx->instruction = i;
            if (!(i % 128u) && !an_continue(ctx))
                return 0;
            if (inst->op == ARCHON_MIR_RV64_VA_START)
                has_va_start = 1;
            if (inst->op != ARCHON_MIR_CALL && inst->op != ARCHON_MIR_CALL_INDIRECT)
                continue;
            if (!calls)
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "missing saved return address for call");
            has_call = 1;
            if (inst->arg_count > AN_MAX_PARAMS)
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid frame call arity");
            call_words = an_argument_slots(fn->types, inst->args, inst->arg_count);
            call_words = call_words > an_abi.argc ? call_words - an_abi.argc : 0;
            if (call_words > outgoing_words)
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "insufficient frame outgoing words for call");
            if (call_words > needed_outgoing)
                needed_outgoing = call_words;
        }
    }
    ctx->block = ctx->instruction = SIZE_MAX;
    if (calls && !has_call)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "unexpected saved return address in leaf frame");
    if (variadic != has_va_start)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "frame does not match va_start use");
    if (needed_phi != phi_count || needed_outgoing != outgoing_words)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "frame capacities do not match function uses");
    return 1;
}

int an_verify_frame(ANContext *ctx, const ArchonMIRFunction *fn, size_t slots,
    size_t phi_count, size_t outgoing_words, int calls, const ANFrame *frame)
{
    const unsigned stack_align = an_abi.stack_align;
    const int variadic = frame->va_start_offset >= 0;
    uintmax_t size, end = 0;
    size_t i;

    ctx->block = ctx->instruction = SIZE_MAX;
    if (stack_align < WordBytes || (stack_align & (stack_align - 1)) ||
        frame->frame_size < 0 || (unsigned)frame->frame_size % stack_align)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid frame size or ABI stack alignment");
    size = (uintmax_t)frame->frame_size;
    if (fn->value_count > AN_MAX_VALUE || fn->param_count > AN_MAX_PARAMS ||
        slots < fn->param_count || slots > fn->value_count)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid frame slot count");
    if (fn->local_count > AN_MAX_VALUE || (fn->local_count && !fn->locals) ||
        fn->block_count > AN_MAX_BLOCK || (fn->block_count && !fn->blocks))
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid frame local or block table");

    /* The frame size and its negation fit int. Check the only displacements
     * above the frame, using division before the multiplication and addition
     * performed in an_parameters. The frame's one-past end is the first word. */
    const size_t named = an_argument_slots(fn->types, NULL, fn->param_count);
    if (named > an_abi.argc &&
        named - an_abi.argc - 1 > ((uintmax_t)INT_MAX - size) / WordBytes)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "incoming stack argument displacement exceeds int");

    if (!words(0, outgoing_words, size, &end))
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid frame outgoing region");
    if (!words(frame->slots_base, slots, size, &end))
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid frame slot region");
    for (i = 0; i < fn->value_count; ++i)
        if (fn->types[i] == ARCHON_MIR_F128 && !words(frame->high_offsets[i], 1, size, &end))
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid f128 high word region");
    if (!words(frame->phi_base, phi_count, size, &end))
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid frame phi region");
    for (i = 0; i < fn->local_count; ++i) {
        const ArchonMIRLocal *local = &fn->locals[i];
        uint32_t align = local->alignment;
        if (!local->size || !align || (align & (align - 1)) || align > stack_align ||
            !region(frame->local_offsets[i], local->size, align, size, &end))
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid frame local region");
    }
    if (calls ? !words(frame->return_offset, 1, size, &end) : frame->return_offset != -1)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid frame saved return address");
    if (variadic && named < an_abi.argc) {
        if (!words(frame->va_start_offset, an_abi.argc - named, size, &end) ||
            end != size)
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "register save area must abut incoming stack arguments");
    } else if (variadic) {
        size_t named_stack = named - an_abi.argc;
        if (named_stack > ((uintmax_t)INT_MAX - size) / WordBytes ||
            (uintmax_t)frame->va_start_offset != size + named_stack * WordBytes)
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid first unnamed stack argument");
    } else if (frame->va_start_offset != -1) {
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid va_start cursor");
    }
    return verify_uses(ctx, fn, phi_count, outgoing_words, calls, variadic,
        slots - fn->param_count);
}
