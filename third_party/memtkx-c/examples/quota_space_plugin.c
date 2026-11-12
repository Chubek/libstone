/* GUIDE 13.2: a space plugin written in C. */
#include <memtkx/memtkx.h>

#include <assert.h>

typedef struct quota_space {
  mtkx_space base; /* first member */
  mtkx_bump_allocator allocator;
  size_t quota;
  size_t charged;
} quota_space;

static mtkx_alloc_result quota_allocate(mtkx_space *space, size_t bytes, size_t alignment) {
  quota_space *q = (quota_space *)space;
  mtkx_alloc_result result;
  if (bytes > q->quota - q->charged) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_OUT_OF_MEMORY);
  }
  result = mtkx_bump_allocator_allocate(&q->allocator, bytes, alignment);
  if (result.ok) {
    q->charged += bytes;
    mtkx_space_record_allocation(space, bytes);
  }
  return result;
}

static const mtkx_space_ops quota_ops = {quota_allocate, NULL};

static double storage[4096 / sizeof(double)];

int main(void) {
  const mtkx_address start = mtkx_as_address(storage);
  quota_space space;
  mtkx_alloc_result fits, over_quota, misaligned;

  mtkx_bump_allocator_init(&space.allocator, start, start + sizeof storage);
  space.quota = 128;
  space.charged = 0;
  if (mtkx_space_init(&space.base, &quota_ops, "quota", MTKX_SPACE_BUMP_POINTER,
                      mtkx_range(start, start + sizeof storage), MTKX_MAX_ALIGN) != MTKX_OK) {
    return 1;
  }

  fits = mtkx_space_allocate(&space.base, 96, 16);
  over_quota = mtkx_space_allocate(&space.base, 64, 16);
  misaligned = mtkx_space_allocate(&space.base, 1, 3);
  assert(fits.ok && !over_quota.ok && space.charged == 96);
  assert(mtkx_space_metrics_of(&space.base)->total_allocations == 1);
  assert(misaligned.error == MTKX_ALLOC_MISALIGNED);

  /* this plugin's reset policy also clears its metrics */
  mtkx_bump_allocator_reset(&space.allocator);
  space.charged = 0;
  mtkx_space_metrics_reset(&space.base.metrics);
  assert(mtkx_space_metrics_of(&space.base)->live_bytes == 0);
  mtkx_space_destroy(&space.base);
  return 0;
}
