/* Port of tests/test_allocator.cpp, extended with space metrics and errors. */
#include <memtkx/memtkx.h>

#include "test_support.h"

static double storage_words[8192 / sizeof(double)];
static double los_words[8192 / sizeof(double)];

int main(void) {
  const mtkx_address base = mtkx_as_address(storage_words);
  const mtkx_address end = base + sizeof storage_words;

  /* --- bump pointer --- */
  {
    mtkx_bump_allocator bump;
    mtkx_alloc_result first, second, too_big;
    mtkx_bump_allocator_init(&bump, base, end);
    first = mtkx_bump_allocator_allocate(&bump, 128, 32);
    CHECK(mtkx_alloc_result_is_ok(first));
    CHECK(mtkx_alloc_result_unwrap(first) % 32 == 0);
    second = mtkx_bump_allocator_allocate(&bump, 64, 8);
    CHECK(mtkx_alloc_result_is_ok(second));
    /* storage is 8-aligned only; padding can precede the first object */
    CHECK(mtkx_bump_allocator_used_bytes(&bump) >= 192);
    CHECK(mtkx_bump_allocator_allocation_count(&bump) == 2);
    CHECK(mtkx_bump_allocator_allocated_bytes(&bump) == 192);
    too_big = mtkx_bump_allocator_allocate(&bump, sizeof storage_words, 1);
    CHECK(mtkx_alloc_result_is_err(too_big));
    CHECK(too_big.error == MTKX_ALLOC_OUT_OF_MEMORY);
    CHECK(mtkx_bump_allocator_allocate(&bump, 0, 8).error == MTKX_ALLOC_ALLOCATION_FAILED);
    CHECK(mtkx_bump_allocator_allocate(&bump, 8, 3).error == MTKX_ALLOC_MISALIGNED);
    /* the inline fast path agrees with the full function */
    {
      mtkx_bump_allocator fast = bump;
      mtkx_bump_allocator full = bump;
      mtkx_address a = mtkx_bump_allocator_try_allocate(&fast, 24, 16);
      mtkx_alloc_result b = mtkx_bump_allocator_allocate(&full, 24, 16);
      CHECK(a != 0 && b.ok && a == b.value);
      CHECK(fast.cursor == full.cursor && fast.allocations == full.allocations);
      CHECK(mtkx_bump_allocator_try_allocate(&fast, 0, 8) == 0);
      CHECK(mtkx_bump_allocator_try_allocate(&fast, 8, 3) == 0);
      CHECK(mtkx_bump_allocator_try_allocate(&fast, sizeof storage_words, 8) == 0);
    }
    mtkx_bump_allocator_reset(&bump);
    CHECK(mtkx_bump_allocator_remaining(&bump) ==
          (size_t)(mtkx_bump_allocator_limit(&bump) - mtkx_bump_allocator_start(&bump)));
    CHECK(mtkx_bump_allocator_used_bytes(&bump) == 0);
    CHECK(mtkx_bump_allocator_allocation_count(&bump) == 2); /* reset keeps counters */
  }

  /* --- bump space --- */
  {
    mtkx_bump_space space;
    CHECK(mtkx_bump_space_init(&space, "arena", base, end) == MTKX_OK);
    CHECK_STR_EQ(mtkx_space_name(&space.base), "arena");
    CHECK(mtkx_space_get_kind(&space.base) == MTKX_SPACE_BUMP_POINTER);
    CHECK_STR_EQ(mtkx_space_kind_string(MTKX_SPACE_BUMP_POINTER), "bump-pointer");
    CHECK(mtkx_alloc_result_is_ok(mtkx_bump_space_allocate(&space, 100, 16)));
    CHECK(mtkx_space_allocate(&space.base, 8, 6).error == MTKX_ALLOC_MISALIGNED);
    CHECK(mtkx_space_metrics_of(&space.base)->live_bytes == 100);
    CHECK(mtkx_space_contains(&space.base, base));
    mtkx_bump_space_reset(&space);
    CHECK(mtkx_space_metrics_of(&space.base)->total_allocations == 1);
    mtkx_bump_space_destroy(&space);
  }

  /* --- free list --- */
  {
    mtkx_free_list_allocator free_list;
    mtkx_alloc_result a, b, c;
    CHECK(mtkx_free_list_allocator_init(&free_list, base, end) == MTKX_OK);
    a = mtkx_free_list_allocator_allocate(&free_list, 1024, 8);
    b = mtkx_free_list_allocator_allocate(&free_list, 512, 16);
    CHECK(mtkx_alloc_result_is_ok(a));
    CHECK(mtkx_alloc_result_is_ok(b));
    CHECK(mtkx_free_list_allocator_free(&free_list, mtkx_alloc_result_unwrap(a), 1024));
    CHECK(mtkx_free_list_allocator_free(&free_list, mtkx_alloc_result_unwrap(b), 512));
    c = mtkx_free_list_allocator_allocate(&free_list, 1500, 8);
    CHECK(mtkx_alloc_result_is_ok(c));
    CHECK(!mtkx_free_list_allocator_free(&free_list, end, 8));
    CHECK(!mtkx_free_list_allocator_free(&free_list, base, 0));
    mtkx_free_list_allocator_destroy(&free_list);
  }

  /* --- fragmentation and coalescing (guide chapter 5 learning task) --- */
  {
    mtkx_free_list_allocator fl;
    mtkx_address x, y, z;
    CHECK(mtkx_free_list_allocator_init(&fl, base, base + 3 * 256) == MTKX_OK);
    x = mtkx_alloc_result_unwrap(mtkx_free_list_allocator_allocate(&fl, 256, 8));
    y = mtkx_alloc_result_unwrap(mtkx_free_list_allocator_allocate(&fl, 256, 8));
    z = mtkx_alloc_result_unwrap(mtkx_free_list_allocator_allocate(&fl, 256, 8));
    CHECK(mtkx_free_list_allocator_free_cells(&fl) == 0);
    CHECK(mtkx_free_list_allocator_free(&fl, x, 256));
    CHECK(mtkx_free_list_allocator_free(&fl, z, 256));
    CHECK(mtkx_free_list_allocator_free_cells(&fl) == 2);
    CHECK(mtkx_free_list_allocator_free_bytes(&fl) == 512);
    CHECK(mtkx_free_list_allocator_allocate(&fl, 512, 8).error == MTKX_ALLOC_OUT_OF_MEMORY);
    CHECK(mtkx_free_list_allocator_free(&fl, y, 256));
    CHECK(mtkx_free_list_allocator_free_cells(&fl) == 1);
    CHECK(mtkx_alloc_result_is_ok(mtkx_free_list_allocator_allocate(&fl, 512, 8)));
    mtkx_free_list_allocator_destroy(&fl);
  }

  /* --- free-list space (guide chapter 5 example) --- */
  {
    mtkx_free_list_space heap;
    mtkx_alloc_result first, second;
    CHECK(mtkx_free_list_space_init(&heap, "objects", base, end) == MTKX_OK);
    first = mtkx_free_list_space_allocate(&heap, 128, 16);
    second = mtkx_free_list_space_allocate(&heap, 256, 16);
    CHECK(first.ok && second.ok);
    CHECK(mtkx_space_metrics_of(&heap.base)->live_bytes == 384);
    CHECK(mtkx_free_list_space_deallocate(&heap, first.value, 128));
    CHECK(mtkx_free_list_space_deallocate(&heap, second.value, 256));
    CHECK(mtkx_space_metrics_of(&heap.base)->live_bytes == 0);
    CHECK(mtkx_free_list_allocator_free_bytes(&heap.allocator) == sizeof storage_words);
    CHECK(mtkx_free_list_allocator_free_cells(&heap.allocator) == 1);
    CHECK(mtkx_free_list_space_reset(&heap) == MTKX_OK);
    mtkx_free_list_space_destroy(&heap);
  }

  /* --- segregated --- */
  {
    mtkx_segregated_allocator segregated;
    mtkx_alloc_result small, medium;
    CHECK(mtkx_segregated_allocator_init(&segregated, base, end) == MTKX_OK);
    CHECK(mtkx_segregated_allocator_class_count(&segregated) == 8);
    small = mtkx_segregated_allocator_allocate(&segregated, 16, 8);
    medium = mtkx_segregated_allocator_allocate(&segregated, 1024, 8);
    CHECK(mtkx_alloc_result_is_ok(small));
    CHECK(mtkx_alloc_result_is_ok(medium));
    CHECK(mtkx_segregated_allocator_free(&segregated, small.value, 16));
    CHECK(mtkx_segregated_allocator_free(&segregated, medium.value, 1024));
    CHECK(mtkx_segregated_allocator_allocate(&segregated, 5000, 8).error ==
          MTKX_ALLOC_OUT_OF_MEMORY);
    mtkx_segregated_allocator_destroy(&segregated);
  }

  /* --- immix --- */
  {
    mtkx_immix_space immix;
    mtkx_alloc_result object;
    CHECK(mtkx_immix_space_init(&immix, MTKX_IMMIX_BLOCK_BYTES * 2) == MTKX_OK);
    object = mtkx_immix_space_allocate(&immix, 128, MTKX_IMMIX_LINE_BYTES);
    CHECK(mtkx_alloc_result_is_ok(object));
    CHECK(mtkx_immix_space_mark(&immix, object.value, 128));
    CHECK(mtkx_immix_space_block_count(&immix) >= 2);
    CHECK(mtkx_immix_space_used_bytes(&immix) >= 128);
    CHECK(mtkx_immix_space_allocate(&immix, MTKX_IMMIX_BLOCK_BYTES + 1, 8).error ==
          MTKX_ALLOC_LARGE_OBJECT_EXCEEDED);
    mtkx_immix_space_destroy(&immix);
  }

  /* --- large objects --- */
  {
    mtkx_los_space los;
    mtkx_alloc_result large, too_small;
    CHECK(mtkx_los_space_init(&los, "los", mtkx_as_address(los_words),
                              mtkx_as_address(los_words) + sizeof los_words, 256) == MTKX_OK);
    large = mtkx_los_space_allocate(&los, 512, 64);
    CHECK(mtkx_alloc_result_is_ok(large));
    too_small = mtkx_los_space_allocate(&los, 128, 8);
    CHECK(mtkx_alloc_result_is_err(too_small));
    CHECK(too_small.error == MTKX_ALLOC_LARGE_OBJECT_EXCEEDED);
    mtkx_los_space_destroy(&los);
  }

  CHECK_STR_EQ(mtkx_alloc_error_string(MTKX_ALLOC_MISALIGNED), "invalid alignment");
  return 0;
}
