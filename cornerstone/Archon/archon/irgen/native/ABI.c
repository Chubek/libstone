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
    size_t i;

    for (i = 0; i < fn->param_count; ++i) {
        unsigned src = AN_T0;
        if (i < an_abi.argc) {
            src = an_abi.args[i];
        } else {
            /* Incoming stack words begin at the caller's sp. */
            an_load(out, src, an_abi.stack_pointer,
                frame->frame_size + stack_arg_offset(i));
        }
        an_write(out, frame, homes, (ArchonValue)i, src);
    }
}

static void call_arguments(ANOut *out, const ArchonMIRInst *inst,
    const ANFrame *frame, const ANHome *homes)
{
    size_t i;

    for (i = an_abi.argc; i < inst->arg_count; ++i) {
        an_read(out, frame, homes, inst->args[i], AN_T0);
        an_store(out, AN_T0, an_abi.stack_pointer, stack_arg_offset(i));
    }
    /* Homes never overlap argument registers, so filling one cannot overwrite
     * a source, even when a value is passed more than once. */
    for (i = 0; i < inst->arg_count && i < an_abi.argc; ++i)
        an_read(out, frame, homes, inst->args[i], an_abi.args[i]);
}

static void call_result(ANOut *out, const ArchonMIRInst *inst,
    const ANFrame *frame, const ANHome *homes)
{
    if (inst->dst != ARCHON_MIR_NONE)
        an_write(out, frame, homes, inst->dst, an_abi.result);
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
