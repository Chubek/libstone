#include "NativeInternal.h"

typedef struct {
    ArchonValue contents;
    uint16_t block_plus_one;
    uint8_t users; /* Saturates at two: only unique vs shared matters. */
} ANVerifiedSlot;

#if AN_MAX_BLOCK > UINT16_MAX
#error Shared-spill verifier needs a wider block index
#endif

/* Independently simulate register and shared spill contents at each use.
 * Shared spills may have neither live-ins nor edge/ABI uses. This checks
 * concrete assignments without trusting FactIR or recomputing intervals. */
static int readable(const ANHome *homes, const ArchonValue *contents,
    const ANVerifiedSlot *slots, size_t block, ArchonValue v)
{
    unsigned reg = homes[v].reg;
    if (reg != AN_ZERO)
        return contents[reg - AN_T4] == v;
    const ANVerifiedSlot *slot = &slots[homes[v].slot];
    return slot->users == 1 || (slot->block_plus_one == block + 1 && slot->contents == v);
}

static int private_slot(const ANHome *homes, const ANVerifiedSlot *slots, ArchonValue v)
{
    return homes[v].reg == AN_ZERO && slots[homes[v].slot].users == 1;
}

static int edge_homes(const ANHome *homes, const ANVerifiedSlot *slots,
    const ArchonMIREdge *edge)
{
    size_t i;
    for (i = 0; i < edge->arg_count; ++i)
        if (!private_slot(homes, slots, edge->args[i]))
            return 0;
    return 1;
}

int an_verify_homes(ANContext *ctx, const ArchonMIRFunction *fn, const ANHome *homes,
    size_t slots)
{
    ANVerifiedSlot used_slots[AN_MAX_VALUE] = {0};
    size_t b, i, j, assigned_slots = 0;
    ctx->block = ctx->instruction = SIZE_MAX;
    if (slots > fn->value_count || slots > AN_MAX_VALUE)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid allocation slot count");
    for (i = 0; i < fn->value_count; ++i) {
        unsigned reg = homes[i].reg;
        if (reg == AN_ZERO) {
            size_t slot = homes[i].slot;
            if (slot >= slots)
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid allocation slot");
            if (!used_slots[slot].users)
                ++assigned_slots;
            if (used_slots[slot].users < 2)
                ++used_slots[slot].users;
        } else if (i < fn->param_count || reg < AN_T4 || reg > AN_T6) {
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid ABI or physical register home");
        }
    }
    if (assigned_slots != slots)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "unused allocation slot");
    for (i = 0; i < fn->param_count; ++i)
        if (!private_slot(homes, used_slots, (ArchonValue)i))
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "ABI parameters must have private stack homes");

    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        const ArchonMIRTerm *term = &block->term;
        ArchonValue contents[AN_T6 - AN_T4 + 1];
        ctx->block = b;
        ctx->instruction = SIZE_MAX;
        if (!an_continue(ctx))
            return 0;
        for (j = 0; j < block->param_count; ++j)
            if (!private_slot(homes, used_slots, block->params[j]))
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "block parameters must have private stack homes");
        for (j = 0; j < sizeof contents / sizeof contents[0]; ++j)
            contents[j] = ARCHON_MIR_NONE;
        for (i = 0; i < block->inst_count; ++i) {
            const ArchonMIRInst *ins = &block->insts[i];
            unsigned shape = an_shape(ins->op);
            ctx->instruction = i;
            if (!(i % 128u) && !an_continue(ctx))
                return 0;
            /* Check the indirect target as a fixed operand, before any
             * call arguments and caller-saved clobber. */
            if (((shape & 2u) && !readable(homes, contents, used_slots, b, ins->a)) ||
                ((shape & 4u) && !readable(homes, contents, used_slots, b, ins->b)))
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "use has no valid allocation home");
            if (an_is_call(ins->op)) {
                for (j = 0; j < ins->arg_count; ++j)
                    if (!readable(homes, contents, used_slots, b, ins->args[j]))
                        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "call argument home was clobbered");
                for (j = 0; j < sizeof contents / sizeof contents[0]; ++j)
                    contents[j] = ARCHON_MIR_NONE;
            }
            if ((shape & 1u) && ins->dst != ARCHON_MIR_NONE) {
                const ANHome *home = &homes[ins->dst];
                if (home->reg != AN_ZERO) {
                    contents[home->reg - AN_T4] = ins->dst;
                } else if (used_slots[home->slot].users > 1) {
                    ANVerifiedSlot *slot = &used_slots[home->slot];
                    if (slot->block_plus_one && slot->block_plus_one != b + 1)
                        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "shared allocation slot crosses blocks");
                    slot->block_plus_one = (uint16_t)(b + 1);
                    slot->contents = ins->dst;
                }
            }
        }
        ctx->instruction = block->inst_count;
        if (term->value != ARCHON_MIR_NONE &&
            !readable(homes, contents, used_slots, b, term->value))
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "terminator home was clobbered");
        if (((term->kind == ARCHON_MIR_BR || term->kind == ARCHON_MIR_CBR) &&
                !edge_homes(homes, used_slots, &term->yes)) ||
            (term->kind == ARCHON_MIR_CBR && !edge_homes(homes, used_slots, &term->no)))
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "edge arguments must have private stack homes");
    }
    ctx->block = ctx->instruction = SIZE_MAX;
    return 1;
}
