#include "NativeInternal.h"

enum { WordBytes = 8 };

static int stack_arg_offset(size_t index)
{
    return (int)((index - an_abi.argc) * WordBytes);
}

size_t an_outgoing_words(size_t argc)
{
    return argc > an_abi.argc ? argc - an_abi.argc : 0;
}

void an_parameters(ANOut *out, const ArchonMIRFunction *fn,
    const ANFrame *frame, const ANHome *homes)
{
    size_t i, cursor = 0;

    for (i = 0; i < fn->param_count; ++i) {
        int wide = fn->types && fn->types[i] == ARCHON_MIR_F128;
        size_t start = an_arg_slot(&cursor, wide);
        for (size_t slot = start; slot < cursor; ++slot) {
            unsigned src = AN_T0;
            if (slot < an_abi.argc)
                src = an_abi.args[slot];
            else
                an_load(out, src, an_frame_base(frame), frame->frame_size + stack_arg_offset(slot));
            if (slot == start)
                an_write(out, frame, homes, (ArchonValue)i, src);
            else
                an_store(out, src, an_frame_base(frame), frame->high_offsets[i]);
        }
    }
}

static void call_arguments(ANOut *out, const ArchonMIRInst *inst,
    const ANFrame *frame, const ANHome *homes)
{
    /* Stack writes first, then populate argument registers. */
    for (int stack = 1; stack >= 0; --stack) {
        size_t cursor = 0;
        for (size_t i = 0; i < inst->arg_count; ++i) {
            ArchonValue v = inst->args[i];
            size_t start = an_arg_slot(&cursor, frame->types && frame->types[v] == ARCHON_MIR_F128);
            for (size_t slot = start; slot < cursor; ++slot) {
                if ((slot >= an_abi.argc) != stack)
                    continue;
                unsigned dst = stack ? AN_T0 : an_abi.args[slot];
                if (slot == start)
                    an_read(out, frame, homes, v, dst);
                else
                    an_load(out, dst, an_frame_base(frame), frame->high_offsets[v]);
                if (stack)
                    an_store(out, dst, an_abi.stack_pointer, stack_arg_offset(slot));
            }
        }
    }
}

static void call_result(ANOut *out, const ArchonMIRInst *inst,
    const ANFrame *frame, const ANHome *homes)
{
    if (inst->dst != ARCHON_MIR_NONE) {
        an_write(out, frame, homes, inst->dst, an_abi.result);
        if (frame->types && frame->types[inst->dst] == ARCHON_MIR_F128)
            an_store(out, an_abi.args[1], an_frame_base(frame), frame->high_offsets[inst->dst]);
    }
}

void an_call(ANOut *out, const ArchonMIRInst *inst,
    const ANFrame *frame, const ANHome *homes)
{
    call_arguments(out, inst, frame, homes);
    an_call_symbol(out, inst->symbol);
    call_result(out, inst, frame, homes);
}

void an_call_indirect(ANOut *out, const ArchonMIRInst *inst,
    const ANFrame *frame, const ANHome *homes)
{
    call_arguments(out, inst, frame, homes);
    /* The signature symbol is verifier metadata, not the jump destination.
     * Fetch the target after argument moves, which may need scratch registers. */
    an_read(out, frame, homes, inst->a, AN_T2);
    an_emit(out, AN_JALR, an_abi.return_address, AN_T2, AN_ZERO, 0);
    call_result(out, inst, frame, homes);
}
