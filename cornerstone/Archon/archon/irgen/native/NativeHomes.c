#include "NativeInternal.h"

/* Independently simulate register contents at every use. Stack locations
 * must be disjoint, and no block may rely on a register live-in. Rather than
 * duplicating the allocator's interval analysis, check the final locations
 * directly against ordered instructions, clobbers and the CFG contract. */
static int readable(const ANHome *homes, const ArchonValue *contents, ArchonValue v)
{
    unsigned reg = homes[v].reg;
    return reg == AN_ZERO || contents[reg - AN_T4] == v;
}

static int edge_homes(const ANHome *homes, const ArchonMIREdge *edge)
{
    size_t i;
    for (i = 0; i < edge->arg_count; ++i)
        if (homes[edge->args[i]].reg != AN_ZERO)
            return 0;
    return 1;
}

int an_verify_homes(ANContext *ctx, const ArchonMIRFunction *fn, const ANHome *homes,
    size_t slots)
{
    unsigned char used_slots[AN_MAX_VALUE] = {0};
    size_t b, i, j, assigned_slots = 0;
    ctx->block = ctx->instruction = SIZE_MAX;
    if (slots > fn->value_count)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid allocation slot count");
    for (i = 0; i < fn->value_count; ++i) {
        unsigned reg = homes[i].reg;
        if (reg == AN_ZERO) {
            size_t slot = homes[i].slot;
            if (slot >= slots || used_slots[slot])
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid or shared allocation slot");
            used_slots[slot] = 1;
            ++assigned_slots;
        } else if (i < fn->param_count || reg < AN_T4 || reg > AN_T6) {
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid ABI or physical register home");
        }
    }
    if (assigned_slots != slots)
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "unused allocation slot");

    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *block = &fn->blocks[b];
        const ArchonMIRTerm *term = &block->term;
        ArchonValue contents[AN_T6 - AN_T4 + 1];
        ctx->block = b;
        ctx->instruction = SIZE_MAX;
        if (!an_continue(ctx))
            return 0;
        for (j = 0; j < block->param_count; ++j)
            if (homes[block->params[j]].reg != AN_ZERO)
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "block parameters must have stack homes");
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
            if (((shape & 2u) && !readable(homes, contents, ins->a)) ||
                ((shape & 4u) && !readable(homes, contents, ins->b)))
                return an_error(ctx, ARCHON_BACKEND_INTERNAL, "use has no valid register home");
            if (an_is_call(ins->op)) {
                for (j = 0; j < ins->arg_count; ++j)
                    if (!readable(homes, contents, ins->args[j]))
                        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "call argument register was clobbered");
                for (j = 0; j < sizeof contents / sizeof contents[0]; ++j)
                    contents[j] = ARCHON_MIR_NONE;
            }
            if ((shape & 1u) && ins->dst != ARCHON_MIR_NONE && homes[ins->dst].reg != AN_ZERO)
                contents[homes[ins->dst].reg - AN_T4] = ins->dst;
        }
        ctx->instruction = block->inst_count;
        if (term->value != ARCHON_MIR_NONE && !readable(homes, contents, term->value))
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "terminator register was clobbered");
        if (((term->kind == ARCHON_MIR_BR || term->kind == ARCHON_MIR_CBR) &&
                !edge_homes(homes, &term->yes)) ||
            (term->kind == ARCHON_MIR_CBR && !edge_homes(homes, &term->no)))
            return an_error(ctx, ARCHON_BACKEND_INTERNAL, "edge arguments must have stack homes");
    }
    ctx->block = ctx->instruction = SIZE_MAX;
    return 1;
}
