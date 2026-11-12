#include "memtkx/space/bump_pointer.h"

void mtkx_bump_allocator_init(mtkx_bump_allocator *allocator, mtkx_address start,
                              mtkx_address end) {
  allocator->allocations = 0;
  allocator->allocated_bytes = 0;
  mtkx_bump_allocator_reset_region(allocator, start, end);
}

void mtkx_bump_allocator_reset_region(mtkx_bump_allocator *allocator, mtkx_address start,
                                      mtkx_address end) {
  allocator->start = mtkx_align_up(start, MTKX_MAX_ALIGN);
  allocator->limit = mtkx_align_down(end, MTKX_MAX_ALIGN);
  allocator->cursor = allocator->start;
}

mtkx_alloc_result mtkx_bump_allocator_allocate(mtkx_bump_allocator *allocator, size_t bytes,
                                               size_t alignment) {
  mtkx_address aligned;
  if (bytes == 0) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
  }
  if (!mtkx_is_power_of_two(alignment)) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_MISALIGNED);
  }
  aligned = mtkx_align_up(allocator->cursor, alignment);
  if (aligned < allocator->cursor ||
      !mtkx_can_allocate(aligned, allocator->limit, bytes, 1)) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_OUT_OF_MEMORY);
  }
  allocator->cursor = aligned + bytes;
  if (allocator->cursor < aligned) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_OUT_OF_MEMORY);
  }
  allocator->allocations += 1;
  allocator->allocated_bytes += bytes;
  return mtkx_alloc_result_from_ok(aligned);
}

/* ------------------------------------------------------------------ */
/*  BumpSpace                                                         */
/* ------------------------------------------------------------------ */

static mtkx_alloc_result mtkx__bump_space_allocate_impl(mtkx_space *base, size_t bytes,
                                                        size_t alignment) {
  mtkx_bump_space *space = (mtkx_bump_space *)base;
  mtkx_alloc_result result = mtkx_bump_allocator_allocate(&space->allocator, bytes, alignment);
  if (result.ok) {
    mtkx_space_record_allocation(base, bytes);
  }
  return result;
}

static const mtkx_space_ops mtkx__bump_space_ops = {mtkx__bump_space_allocate_impl, NULL};

mtkx_status mtkx_bump_space_init(mtkx_bump_space *space, const char *name,
                                 mtkx_address start, mtkx_address end) {
  mtkx_bump_allocator_init(&space->allocator, start, end);
  return mtkx_space_init(&space->base, &mtkx__bump_space_ops, name, MTKX_SPACE_BUMP_POINTER,
                         mtkx_range(start, end), MTKX_MAX_ALIGN);
}

mtkx_alloc_result mtkx_bump_space_allocate(mtkx_bump_space *space, size_t bytes,
                                           size_t alignment) {
  return mtkx_space_allocate(&space->base, bytes, alignment);
}

void mtkx_bump_space_destroy(mtkx_bump_space *space) { mtkx_space_destroy(&space->base); }
