/* Port of tests/test_large_object.cpp plus the guide's chapter 6.4 example. */
#include <memtkx/memtkx.h>

#include "test_support.h"

static double small_words[2048 / sizeof(double)];
static double big_words[16384 / sizeof(double)];

int main(void) {
  mtkx_los_space los;
  mtkx_alloc_result allocation, too_small;
  mtkx_address object;

  CHECK(mtkx_los_space_init(&los, "los", mtkx_as_address(small_words),
                            mtkx_as_address(small_words) + sizeof small_words,
                            256) == MTKX_OK);
  allocation = mtkx_los_space_allocate(&los, 512, 64);
  CHECK(mtkx_alloc_result_is_ok(allocation));
  object = mtkx_alloc_result_unwrap(allocation);

  CHECK(mtkx_los_space_deallocate(&los, object, 512));
  CHECK(mtkx_space_metrics_of(&los.base)->live_bytes == 0);
  CHECK(!mtkx_los_space_deallocate(&los, 0, 128));
  CHECK(!mtkx_los_space_deallocate(&los, object, 0));

  too_small = mtkx_los_space_allocate(&los, 128, 64);
  CHECK(mtkx_alloc_result_is_err(too_small));
  CHECK(mtkx_los_space_allocate(&los, sizeof small_words + 1, 8).error ==
        MTKX_ALLOC_LARGE_OBJECT_EXCEEDED);
  mtkx_los_space_destroy(&los);

  {
    mtkx_los_space large;
    mtkx_alloc_result big;
    size_t capacity_after_allocation;
    const mtkx_address start = mtkx_as_address(big_words);
    CHECK(mtkx_los_space_init(&large, "large", start, start + sizeof big_words,
                              MTKX_DEFAULT_LARGE_OBJECT_THRESHOLD) == MTKX_OK);
    CHECK(mtkx_los_space_threshold(&large) == 4096);
    CHECK(mtkx_alloc_result_is_err(mtkx_los_space_allocate(&large, 128, 16)));
    big = mtkx_los_space_allocate(&large, 8192, 16);
    CHECK(big.ok);
    capacity_after_allocation = mtkx_bump_allocator_remaining(&large.allocator);
    CHECK(mtkx_los_space_deallocate(&large, big.value, 8192));
    CHECK(mtkx_space_metrics_of(&large.base)->live_bytes == 0);
    CHECK(mtkx_bump_allocator_remaining(&large.allocator) == capacity_after_allocation);
    mtkx_los_space_reset(&large);
    CHECK(mtkx_bump_allocator_used_bytes(&large.allocator) == 0);
    mtkx_los_space_destroy(&large);
  }
  return 0;
}
