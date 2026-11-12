#include "memtkx/space/los_space.h"

static mtkx_alloc_result mtkx__los_allocate_impl(mtkx_space *base, size_t bytes,
                                                 size_t alignment) {
  mtkx_los_space *space = (mtkx_los_space *)base;
  mtkx_alloc_result result;
  if (bytes < space->threshold) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_LARGE_OBJECT_EXCEEDED);
  }
  if (bytes > mtkx_range_size(mtkx_space_region(base))) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_LARGE_OBJECT_EXCEEDED);
  }
  result = mtkx_bump_allocator_allocate(&space->allocator, bytes, alignment);
  if (result.ok) {
    mtkx_space_record_allocation(base, bytes);
  }
  return result;
}

static const mtkx_space_ops mtkx__los_ops = {mtkx__los_allocate_impl, NULL};

mtkx_status mtkx_los_space_init(mtkx_los_space *space, const char *name, mtkx_address start,
                                mtkx_address end, size_t threshold) {
  mtkx_bump_allocator_init(&space->allocator, start, end);
  space->threshold = threshold;
  return mtkx_space_init(&space->base, &mtkx__los_ops, name, MTKX_SPACE_LARGE_OBJECT,
                         mtkx_range(start, end), MTKX_MAX_ALIGN);
}

mtkx_alloc_result mtkx_los_space_allocate(mtkx_los_space *space, size_t bytes,
                                          size_t alignment) {
  return mtkx_space_allocate(&space->base, bytes, alignment);
}

bool mtkx_los_space_deallocate(mtkx_los_space *space, mtkx_address address, size_t bytes) {
  mtkx_address_range region = mtkx_space_region(&space->base);
  if (address < region.start || address >= region.end || bytes == 0 ||
      !mtkx_range_contains(region, address)) {
    return false;
  }
  if (bytes > mtkx_range_size(region)) {
    return false;
  }
  mtkx_space_record_deallocation(&space->base, bytes);
  return true;
}

void mtkx_los_space_destroy(mtkx_los_space *space) { mtkx_space_destroy(&space->base); }
