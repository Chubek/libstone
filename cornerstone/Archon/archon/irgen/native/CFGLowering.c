#include "NativeInternal.h"

enum { WordBytes = 8 };

void an_edge(ANOut *out, size_t fn, const ArchonMIRFunction *f,
    const ANFrame *frame, const ANHome *homes, const ArchonMIREdge *edge)
{
    const ArchonMIRBlock *target = &f->blocks[edge->target];
    size_t i;

    /* Capture every argument before overwriting any destination. A back edge
     * can permute the very same homes it reads. */
    for (i = 0; i < edge->arg_count; ++i) {
        an_read(out, frame, homes, edge->args[i], AN_T0);
        an_store(out, AN_T0, an_abi.stack_pointer,
            frame->phi_base + (int)i * WordBytes);
    }
    for (i = 0; i < edge->arg_count; ++i) {
        an_load(out, AN_T0, an_abi.stack_pointer,
            frame->phi_base + (int)i * WordBytes);
        an_write(out, frame, homes, target->params[i], AN_T0);
    }
    an_jump(out, fn, edge->target);
}

void an_terminator(ANOut *out, size_t fn, const ArchonMIRFunction *f,
    const ANFrame *frame, const ANHome *homes, const ArchonMIRTerm *term,
    unsigned *next_label)
{
    switch (term->kind) {
    case ARCHON_MIR_RETURN:
        if (term->value != ARCHON_MIR_NONE)
            an_read(out, frame, homes, term->value, an_abi.result);
        an_leave(out, frame);
        break;
    case ARCHON_MIR_BR:
        an_edge(out, fn, f, frame, homes, &term->yes);
        break;
    case ARCHON_MIR_CBR: {
        unsigned yes = (*next_label)++;
        an_read(out, frame, homes, term->value, AN_T0);
        an_branch(out, 1, AN_T0, AN_ZERO, fn, yes);
        an_edge(out, fn, f, frame, homes, &term->no);
        an_label(out, fn, yes);
        an_edge(out, fn, f, frame, homes, &term->yes);
        break;
    }
    case ARCHON_MIR_TRAP: {
        unsigned halt = (*next_label)++;
        an_emit(out, AN_EBREAK, AN_ZERO, AN_ZERO, AN_ZERO, 0);
        an_label(out, fn, halt);
        an_jump(out, fn, halt);
        break;
    }
    default:
        if (out->status == ARCHON_BACKEND_OK)
            out->status = ARCHON_BACKEND_INTERNAL;
        break;
    }
}
