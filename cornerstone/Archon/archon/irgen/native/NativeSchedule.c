#include "NativeInternal.h"

#include <stdlib.h>
#include <string.h>

/* The only operations allowed to move. In particular, MIR shifts mask
 * their counts and unchecked division/remainder define every input case.
 * A COPY is total for any verified scalar value type. */
static int movable(ArchonMIROpcode op)
{
    switch (op) {
    case ARCHON_MIR_CONST:
    case ARCHON_MIR_COPY:
    case ARCHON_MIR_ADD:
    case ARCHON_MIR_SUB:
    case ARCHON_MIR_MUL:
    case ARCHON_MIR_DIV_S:
    case ARCHON_MIR_REM_S:
    case ARCHON_MIR_DIV_U:
    case ARCHON_MIR_REM_U:
    case ARCHON_MIR_AND:
    case ARCHON_MIR_OR:
    case ARCHON_MIR_XOR:
    case ARCHON_MIR_SHL:
    case ARCHON_MIR_LSHR:
    case ARCHON_MIR_ASHR:
    case ARCHON_MIR_EQ:
    case ARCHON_MIR_SLT:
    case ARCHON_MIR_ULT:
        return 1;
    default:
        return 0;
    }
}

static int input_ok(ANContext *ctx, const ArchonMIRBlock *block)
{
    ctx->instruction = SIZE_MAX;
    if (!an_continue(ctx))
        return 0;
    if (!block || (block->inst_count && !block->insts))
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "missing schedule input");
    if (block->inst_count > AN_MAX_INSTS)
        return an_error(ctx, ARCHON_BACKEND_LIMIT, "block scheduling instruction limit");
    return 1;
}

static int use_before(ANContext *ctx, const uint16_t *position, ArchonValue v, size_t i)
{
    if (v == ARCHON_MIR_NONE)
        return 1;
    if (v >= AN_MAX_VALUE)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "schedule operand out of range");
    if (position[v] > i) /* Positions are one-based; zero is a live-in. */
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "schedule use precedes its definition");
    return 1;
}

/* Independently check the index plan, not its construction or priority.
 * Selecting original indices makes operand or opcode mutation impossible. */
int an_verify_schedule(ANContext *ctx, const ArchonMIRBlock *block,
    const size_t *order, size_t count)
{
    uint64_t seen[(AN_MAX_INSTS + 63u) / 64u] = {0};
    uint16_t position[AN_MAX_VALUE] = {0};
    size_t i, begin = 0, end = 0;

    if (!input_ok(ctx, block))
        return 0;
    if (count != block->inst_count || (count && !order))
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "wrong schedule length");

    for (i = 0; i < count; ++i) {
        const size_t src = order[i];
        uint64_t bit;
        ArchonValue dst;
        ctx->instruction = i;
        if (!an_continue(ctx))
            return 0;
        if (i == end) {
            begin = i;
            /* Nonmovable operations are singleton regions. */
            end = i + 1;
            if (movable(block->insts[i].op))
                while (end < count && movable(block->insts[end].op))
                    ++end;
        }
        if (src < begin || src >= end)
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "schedule crosses an instruction boundary");
        bit = UINT64_C(1) << (src % 64u);
        if (seen[src / 64u] & bit)
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "repeated schedule instruction");
        seen[src / 64u] |= bit;
        dst = block->insts[src].dst;
        if (dst != ARCHON_MIR_NONE || movable(block->insts[src].op)) {
            if (dst >= AN_MAX_VALUE || position[dst])
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid or repeated schedule definition");
            position[dst] = (uint16_t)(i + 1);
        }
    }
    for (i = 0; i < count; ++i) {
        const ArchonMIRInst *inst = &block->insts[order[i]];
        size_t j;
        ctx->instruction = i;
        if (!an_continue(ctx) ||
            !use_before(ctx, position, inst->a, i) ||
            !use_before(ctx, position, inst->b, i))
            return 0;
        if (an_is_call(inst->op)) {
            if (inst->arg_count > AN_MAX_PARAMS || (inst->arg_count && !inst->args))
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid schedule call arguments");
            for (j = 0; j < inst->arg_count; ++j)
                if (!use_before(ctx, position, inst->args[j], i))
                    return 0;
        }
    }
    ctx->instruction = SIZE_MAX;
    return 1;
}

static int prioritize_dependency(ANContext *ctx, const uint16_t *def,
    uint16_t *depth, size_t begin, size_t use, ArchonValue operand)
{
    size_t source;
    unsigned priority;
    if (operand == ARCHON_MIR_NONE)
        return 1;
    if (operand >= AN_MAX_VALUE)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "schedule operand out of range");
    source = def[operand]; /* One-based, zero if not defined in a movable region. */
    if (source <= begin)
        return 1;
    if (source > use)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "schedule input is not in SSA order");
    priority = 1u + depth[use - begin];
    if (priority > depth[source - 1u - begin])
        depth[source - 1u - begin] = (uint16_t)priority;
    return 1;
}

static int preferred(const ANContext *ctx, ArchonValue dst)
{
    if (ctx->facts)
        return an_fact(ctx, ARCHON_BACKEND_FACT_OBSERVABLE_INPUT, dst);
    return ctx->schedule_hints && ctx->schedule_hints->prefer &&
        ctx->schedule_hints->prefer(ctx->schedule_hints->user, ctx->function, ctx->block, dst) != 0;
}

static int schedule_region(ANContext *ctx, const ArchonMIRInst *insts,
    size_t begin, size_t end, uint16_t *def, size_t *order)
{
    uint16_t depth[AN_MAX_VALUE], priority[AN_MAX_VALUE], bucket[2u * AN_MAX_VALUE + 2u];
    const size_t length = end - begin;
    size_t i, next = begin;

    if (length > AN_MAX_VALUE)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "too many SSA definitions in schedule region");
    memset(bucket, 0, (2u * length + 2u) * sizeof(bucket[0]));
    for (i = begin; i < end; ++i) {
        const ArchonValue dst = insts[i].dst;
        ctx->instruction = i;
        if (!an_continue(ctx))
            return 0;
        if (dst >= AN_MAX_VALUE || def[dst])
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid schedule input definition");
        def[dst] = (uint16_t)(i + 1);
        depth[i - begin] = 1;
    }
    for (i = end; i > begin;) {
        const ArchonMIRInst *inst = &insts[--i];
        ctx->instruction = i;
        if (!an_continue(ctx) ||
            !prioritize_dependency(ctx, def, depth, begin, i, inst->a) ||
            !prioritize_dependency(ctx, def, depth, begin, i, inst->b))
            return 0;
        priority[i - begin] = (uint16_t)(2u * depth[i - begin] + preferred(ctx, inst->dst));
        ++bucket[priority[i - begin]];
    }
    /* List scheduling by longest local def-use chain, optional preference
     * for equal-depth instructions, then original index.
     * Every predecessor has strictly higher depth than its users. Thus the
     * next highest-priority instruction is always ready, and stable buckets
     * implement the ready-list choice without maintaining a graph or heap. */
    for (i = 2u * length + 1u; i; --i) {
        size_t size = bucket[i];
        bucket[i] = (uint16_t)next;
        next += size;
    }
    for (i = begin; i < end; ++i)
        order[bucket[priority[i - begin]]++] = i;
    return 1;
}

/* Input is verified SSA. Output has block->inst_count entries and must not
 * overlap the input array. Nothing is written until the plan is verified. */
int an_schedule_block(ANContext *ctx, const ArchonMIRBlock *block, ArchonMIRInst *out)
{
    uint16_t def[AN_MAX_VALUE] = {0};
    size_t i, *order;
    int ok = 0;

    if (!input_ok(ctx, block))
        return 0;
    if (!block->inst_count)
        return 1;
    if (!out || out == block->insts)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "schedule requires a separate output buffer");
    order = malloc(block->inst_count * sizeof(*order));
    if (!order)
        return an_error(ctx, ARCHON_BACKEND_NOMEM, "allocating schedule plan");
    for (i = 0; i < block->inst_count;) {
        size_t end = i + 1;
        ctx->instruction = i;
        if (!an_continue(ctx))
            goto done;
        if (movable(block->insts[i].op)) {
            while (end < block->inst_count && movable(block->insts[end].op))
                ++end;
            if (!schedule_region(ctx, block->insts, i, end, def, order))
                goto done;
        } else
            order[i] = i;
        i = end;
    }
    if (!an_verify_schedule(ctx, block, order, block->inst_count) || !an_continue(ctx))
        goto done;
    for (i = 0; i < block->inst_count; ++i)
        out[i] = block->insts[order[i]];
    ok = 1;
done:
    free(order);
    return ok;
}
