#include "NativeInternal.h"

#include <limits.h>

enum { WordBytes = 8 };

static size_t align_up(size_t size, size_t alignment)
{
    return (size + alignment - 1) & ~(alignment - 1);
}

static int add_words(size_t *size, size_t words, size_t limit)
{
    if (words > (limit - *size) / WordBytes)
        return 0;
    *size += words * WordBytes;
    return 1;
}

int an_frame(ANContext *ctx, const ArchonMIRFunction *fn, size_t slots,
    size_t phi_count, size_t outgoing_words, int calls, ANFrame *frame)
{
    const size_t stack_align = an_abi.stack_align;
    size_t limit, last_incoming = 0, size = 0, i;

    if (stack_align < WordBytes || (stack_align & (stack_align - 1)))
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "invalid ABI stack alignment");
    if (fn->param_count > AN_MAX_PARAMS)
        return an_error(ctx, ARCHON_BACKEND_LIMIT, "too many frame parameters");
    /* Only the last incoming stack argument constrains the upper end of
     * the displacement range. The first is at frame_size itself. */
    if (fn->param_count > an_abi.argc)
        last_incoming = (fn->param_count - an_abi.argc - 1) * WordBytes;
    limit = ((size_t)INT_MAX - last_incoming) & ~(stack_align - 1);
    if (fn->local_count > AN_MAX_VALUE)
        return an_error(ctx, ARCHON_BACKEND_LIMIT, "too many frame objects");
    if (!add_words(&size, outgoing_words, limit))
        goto too_large;
    frame->slots_base = (int)size;
    if (!add_words(&size, slots, limit))
        goto too_large;
    frame->phi_base = (int)size;
    if (!add_words(&size, phi_count, limit))
        goto too_large;

    for (i = 0; i < fn->local_count; ++i) {
        const ArchonMIRLocal *local = &fn->locals[i];
        uint32_t alignment = local->alignment;
        if (!local->size || !alignment || (alignment & (alignment - 1)))
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                "invalid frame object size or alignment");
        if (alignment > stack_align)
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED,
                "frame object exceeds ABI stack alignment");
        size = align_up(size, alignment);
        if (local->size > limit - size)
            goto too_large;
        frame->local_offsets[i] = (int)size;
        size += local->size;
    }

    frame->return_offset = -1;
    if (calls) {
        size = align_up(size, WordBytes);
        frame->return_offset = (int)size;
        if (!add_words(&size, 1, limit))
            goto too_large;
    }
    frame->frame_size = (int)align_up(size, stack_align);
    return 1;

too_large:
    return an_error(ctx, ARCHON_BACKEND_LIMIT, "stack frame exceeds displacement limit");
}

void an_li(ANOut *out, unsigned dst, uint64_t value)
{
    int low = (int)(value & UINT64_C(4095));
    if (low >= 2048)
        low -= 4096;
    if (value <= UINT64_C(2047) || value >= UINT64_MAX - UINT64_C(2047)) {
        an_emit(out, AN_ADDI, dst, AN_ZERO, 0, low);
    } else {
        /* Arithmetic shift of the bit pattern, without shifting a signed
         * value or converting a large uint64_t to a signed integer. Round the
         * upper part up when the low 12-bit digit will be negative. */
        uint64_t high = (value >> 12) | ((value >> 63) ? (UINT64_MAX << 52) : 0);
        if (low < 0)
            ++high;
        an_li(out, dst, high);
        an_emit(out, AN_SLLI, dst, dst, 0, 12);
        if (low)
            an_emit(out, AN_ADDI, dst, dst, 0, low);
    }
}

static int short_disp(int offset)
{
    return offset >= -2048 && offset <= 2047;
}

static void scratch_conflict(ANOut *out)
{
    /* x28 is reserved; a caller must not keep an operand there if the
     * expansion needs it to materialize the displacement. */
    if (out->status == ARCHON_BACKEND_OK)
        out->status = ARCHON_BACKEND_INTERNAL;
}

void an_addr(ANOut *out, unsigned dst, unsigned base, int offset)
{
    if (dst == AN_ZERO || (dst == base && !offset))
        return;
    if (short_disp(offset)) {
        an_emit(out, AN_ADDI, dst, base, 0, offset);
    } else {
        unsigned tmp = dst == base ? AN_T3 : dst;
        if (tmp == base) {
            scratch_conflict(out);
            return;
        }
        an_li(out, tmp, (uint64_t)offset);
        an_emit(out, AN_ADD, dst, base, tmp, 0);
    }
}

void an_load(ANOut *out, unsigned dst, unsigned base, int offset)
{
    if (!short_disp(offset)) {
        unsigned tmp = dst == AN_ZERO || dst == base ? AN_T3 : dst;
        an_addr(out, tmp, base, offset);
        base = tmp;
        offset = 0;
    }
    an_emit(out, AN_LD, dst, base, 0, offset);
}

void an_store(ANOut *out, unsigned src, unsigned base, int offset)
{
    if (!short_disp(offset)) {
        if (src == AN_T3 || base == AN_T3) {
            scratch_conflict(out);
            return;
        }
        an_addr(out, AN_T3, base, offset);
        base = AN_T3;
        offset = 0;
    }
    an_emit(out, AN_SD, src, base, 0, offset);
}

void an_enter(ANOut *out, const ANFrame *frame)
{
    unsigned sp = an_abi.stack_pointer;
    an_addr(out, sp, sp, -frame->frame_size);
    if (frame->return_offset >= 0)
        an_store(out, an_abi.return_address, sp, frame->return_offset);
}

void an_leave(ANOut *out, const ANFrame *frame)
{
    unsigned sp = an_abi.stack_pointer, ra = an_abi.return_address;
    if (frame->return_offset >= 0)
        an_load(out, ra, sp, frame->return_offset);
    an_addr(out, sp, sp, frame->frame_size);
    an_emit(out, AN_JALR, AN_ZERO, ra, 0, 0);
}

static int register_home(const ANHome *home)
{
    return home->reg >= AN_T4 && home->reg <= AN_T6;
}

static int slot_offset(const ANFrame *frame, const ANHome *home)
{
    return frame->slots_base + (int)((size_t)home->slot * WordBytes);
}

void an_read(ANOut *out, const ANFrame *frame, const ANHome *homes,
    ArchonValue value, unsigned dst)
{
    const ANHome *home = &homes[value];
    if (register_home(home))
        an_addr(out, dst, home->reg, 0);
    else
        an_load(out, dst, an_abi.stack_pointer, slot_offset(frame, home));
}

void an_write(ANOut *out, const ANFrame *frame, const ANHome *homes,
    ArchonValue value, unsigned src)
{
    const ANHome *home = &homes[value];
    if (register_home(home))
        an_addr(out, home->reg, src, 0);
    else
        an_store(out, src, an_abi.stack_pointer, slot_offset(frame, home));
}
