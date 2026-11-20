#include "NativeInternal.h"

void an_convert(ANOut *out, ArchonMIROpcode op, unsigned dst, unsigned src,
    unsigned count_scratch, unsigned width)
{
    int shift;

    if (out->status != ARCHON_BACKEND_OK)
        return;
    if (width < 1 || width > 64 ||
        (op != ARCHON_MIR_TRUNC && op != ARCHON_MIR_ZEXT && op != ARCHON_MIR_SEXT)) {
        out->status = ARCHON_BACKEND_INTERNAL;
        return;
    }
    if (width == 64) {
        an_emit(out, AN_ADDI, dst, src, AN_ZERO, 0);
        return;
    }
    if (count_scratch == AN_ZERO || count_scratch == dst) {
        out->status = ARCHON_BACKEND_INTERNAL;
        return;
    }

    /* Drop the high input bits, moving the logical sign bit to bit 63. */
    shift = 64 - (int)width;
    an_emit(out, AN_SLLI, dst, src, AN_ZERO, shift);
    an_emit(out, AN_ADDI, count_scratch, AN_ZERO, AN_ZERO, shift);
    an_emit(out, op == ARCHON_MIR_SEXT ? AN_SRA : AN_SRL, dst, dst, count_scratch, 0);
}
