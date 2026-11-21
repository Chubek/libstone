#include "NativeInternal.h"

#include <stdlib.h>

typedef struct {
    size_t block, definition, end, reads;
    unsigned reg;
    unsigned force_stack : 1;
    unsigned fixed_slot : 1;
    unsigned demand : 1;
} ANRange;

static ArchonValue defined_value(const ArchonMIRInst *inst)
{
    return (an_shape(inst->op) & 1u) ? inst->dst : ARCHON_MIR_NONE;
}

static int begin_block(ANContext *ctx, size_t block)
{
    ctx->block = block;
    ctx->instruction = SIZE_MAX;
    return an_continue(ctx);
}

static int poll_instruction(ANContext *ctx, size_t i)
{
    /* Calls and edges have bounded operand lists, so polling by instruction
     * keeps cancellation responsive even in a single large block. */
    if (i % 256u != 0)
        return 1;
    ctx->instruction = i;
    return an_continue(ctx);
}

static int find_definitions(ANContext *ctx, const ArchonMIRFunction *fn,
    ANRange *ranges)
{
    size_t b, i;

    for (i = 0; i < fn->param_count; ++i)
        ranges[i].force_stack = ranges[i].fixed_slot = 1;
    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        if (!begin_block(ctx, b))
            return 0;
        for (i = 0; i < block->param_count; ++i)
            ranges[block->params[i]].force_stack = ranges[block->params[i]].fixed_slot = 1;
        for (i = 0; i < block->inst_count; ++i) {
            ArchonValue dst = defined_value(&block->insts[i]);
            if (!poll_instruction(ctx, i))
                return 0;
            if (dst != ARCHON_MIR_NONE) {
                ranges[dst].block = b;
                ranges[dst].definition = ranges[dst].end = i;
                ranges[dst].demand = an_fact_at(ctx, ARCHON_BACKEND_FACT_OBSERVABLE_INPUT, b, dst);
            }
        }
    }
    return 1;
}

static void note_use(ANRange *ranges, ArchonValue value, size_t b, size_t i,
    size_t last_call)
{
    ANRange *range;
    if (value == ARCHON_MIR_NONE)
        return;
    range = &ranges[value];
    if (range->fixed_slot)
        return;
    if (range->block != b) {
        range->force_stack = range->fixed_slot = 1;
        return;
    }
    if (last_call != SIZE_MAX && last_call > range->definition)
        range->force_stack = 1;
    range->end = i;
    ++range->reads;
}

static void force_edge_homes(ANRange *ranges, const ArchonMIREdge *edge)
{
    size_t i;
    for (i = 0; i < edge->arg_count; ++i)
        ranges[edge->args[i]].force_stack = ranges[edge->args[i]].fixed_slot = 1;
}

static int prefer_incoming(const ANRange *incoming, const ANRange *victim)
{
    /* A positive demand fact nominates a range for traffic-aware ranking;
     * actual MIR reads, not the fact, estimate its spill cost. Missing
     * demand never implies a value has no users. Preserve the original
     * farthest-end decision when neither range has a better qualified cost. */
    if (incoming->demand && incoming->reads > victim->reads)
        return 1;
    if (victim->demand && victim->reads > incoming->reads)
        return 0;
    return incoming->end < victim->end;
}

static int find_uses(ANContext *ctx, const ArchonMIRFunction *fn, ANRange *ranges)
{
    size_t b, i, arg;

    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        const ArchonMIRTerm *term = &block->term;
        size_t last_call = SIZE_MAX;
        if (!begin_block(ctx, b))
            return 0;
        for (i = 0; i < block->inst_count; ++i) {
            const ArchonMIRInst *inst = &block->insts[i];
            unsigned shape = an_shape(inst->op);
            if (!poll_instruction(ctx, i))
                return 0;
            if (shape & 2u)
                note_use(ranges, inst->a, b, i, last_call);
            if (shape & 4u)
                note_use(ranges, inst->b, b, i, last_call);
            if (an_is_call(inst->op)) {
                /* The indirect target and all arguments are read before
                 * the clobber. The result starts after it. */
                for (arg = 0; arg < inst->arg_count; ++arg)
                    note_use(ranges, inst->args[arg], b, i, last_call);
                last_call = i;
            }
        }
        if (term->kind == ARCHON_MIR_RETURN || term->kind == ARCHON_MIR_CBR)
            note_use(ranges, term->value, b, i, last_call);
        if (term->kind == ARCHON_MIR_BR || term->kind == ARCHON_MIR_CBR)
            force_edge_homes(ranges, &term->yes);
        if (term->kind == ARCHON_MIR_CBR)
            force_edge_homes(ranges, &term->no);
    }
    return 1;
}

static int assign_registers(ANContext *ctx, const ArchonMIRFunction *fn,
    ANRange *ranges)
{
    size_t b, i;

    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        ANRange *active[AN_T6 - AN_T4 + 1] = {0};
        if (!begin_block(ctx, b))
            return 0;
        for (i = 0; i < block->inst_count; ++i) {
            ArchonValue dst = defined_value(&block->insts[i]);
            ANRange *incoming, *victim = NULL;
            unsigned r;
            if (!poll_instruction(ctx, i))
                return 0;
            if (dst == ARCHON_MIR_NONE || ranges[dst].force_stack)
                continue;
            incoming = &ranges[dst];
            for (r = AN_T4; r <= AN_T6; ++r) {
                ANRange *current = active[r - AN_T4];
                /* Reads at i precede the write at i. Prefer the first
                 * free register, or the first of equally late victims. */
                if (!current || current->end <= i)
                    break;
                if (!victim || current->end > victim->end)
                    victim = current;
            }
            if (r > AN_T6) {
                if (!prefer_incoming(incoming, victim))
                    continue;
                /* This is an offline decision: the victim's definition
                 * and all of its uses will use its stack home. */
                r = victim->reg;
                victim->reg = AN_ZERO;
            }
            active[r - AN_T4] = incoming;
            incoming->reg = r;
        }
    }
    return 1;
}

static int reusable(const ANRange *range)
{
    return range->reg == AN_ZERO && !range->fixed_slot && range->demand && range->reads;
}

static int assign_slots(ANContext *ctx, const ArchonMIRFunction *fn,
    const ANRange *ranges, ANHome *homes, size_t *slots)
{
    /* Compact at the end in value order, so absent a reuse the layout
     * matches the baseline (including its short-displacement offsets). */
    uint32_t compact[AN_MAX_VALUE] = {0};
    size_t ends[AN_MAX_VALUE], count = 0, final = 0, b, i;
    int candidates = 0;
    for (i = 0; i < fn->value_count; ++i) {
        homes[i].reg = ranges[i].reg;
        homes[i].slot = 0;
        if (ranges[i].reg == AN_ZERO) {
            if (reusable(&ranges[i]))
                candidates = 1;
            else
                homes[i].slot = (uint32_t)count++;
        }
    }
    for (b = 0; candidates && b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        size_t base = count;
        if (!begin_block(ctx, b))
            return 0;
        for (i = 0; i < block->inst_count; ++i) {
            ArchonValue dst = defined_value(&block->insts[i]);
            size_t slot;
            if (!poll_instruction(ctx, i))
                return 0;
            if (dst == ARCHON_MIR_NONE || !reusable(&ranges[dst]))
                continue;
            /* Only reuse a spill home from this block whose last read is
             * strictly before this definition. Calls do not write these
             * private slots; outgoing arguments occupy a separate region. */
            for (slot = base; slot < count && ends[slot] >= i; ++slot)
                ;
            if (slot == count)
                ++count;
            ends[slot] = ranges[dst].end;
            homes[dst].slot = (uint32_t)slot;
        }
    }
    ctx->block = ctx->instruction = SIZE_MAX;
    if (!an_continue(ctx))
        return 0;
    if (candidates) {
        for (i = 0; i < fn->value_count; ++i) {
            uint32_t slot = homes[i].slot;
            if (homes[i].reg != AN_ZERO)
                continue;
            if (!compact[slot])
                compact[slot] = (uint32_t)++final;
            homes[i].slot = compact[slot] - 1;
        }
    }
    *slots = candidates ? final : count;
    return 1;
}

int an_allocate(ANContext *ctx, const ArchonMIRFunction *fn, ANHome *homes,
    size_t *slots)
{
    ANRange *ranges;
    int ok;

    ctx->stage = ARCHON_BACKEND_ALLOCATE;
    ctx->block = ctx->instruction = SIZE_MAX;
    if (!an_continue(ctx))
        return 0;
    /* The verifier has established unique definitions, valid use sites,
     * and bounded counts. */
    if (fn->value_count > AN_MAX_VALUE)
        return an_error(ctx, ARCHON_BACKEND_LIMIT, "too many native register homes");
    if (fn->value_count == 0) {
        *slots = 0;
        return 1;
    }
    ranges = calloc(fn->value_count, sizeof(*ranges));
    if (!ranges)
        return an_error(ctx, ARCHON_BACKEND_NOMEM, "allocating native live ranges");

    ok = find_definitions(ctx, fn, ranges) && find_uses(ctx, fn, ranges) &&
        assign_registers(ctx, fn, ranges) && assign_slots(ctx, fn, ranges, homes, slots);
    free(ranges);
    return ok;
}
