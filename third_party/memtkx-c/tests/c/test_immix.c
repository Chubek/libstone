/* Port of tests/test_immix.cpp plus the guide's marking experiment. */
#include <memtkx/memtkx.h>

#include "test_support.h"

int main(void) {
  const size_t requested = MTKX_IMMIX_BLOCK_BYTES + 1;
  mtkx_immix_space space;
  mtkx_alloc_result head, tail;

  CHECK(mtkx_immix_space_init(&space, requested) == MTKX_OK);
  head = mtkx_immix_space_allocate(&space, MTKX_IMMIX_BLOCK_BYTES, MTKX_IMMIX_LINE_BYTES);
  tail = mtkx_immix_space_allocate(&space, 1, MTKX_IMMIX_LINE_BYTES);
  CHECK(mtkx_alloc_result_is_ok(head));
  CHECK(mtkx_alloc_result_is_ok(tail));
  CHECK(mtkx_immix_space_block_count(&space) >= 2);
  CHECK(mtkx_immix_space_total_bytes(&space) >= requested);
  CHECK(mtkx_range_size(mtkx_space_region(&space.base)) ==
        mtkx_immix_space_block_count(&space) * MTKX_IMMIX_BLOCK_BYTES);

  CHECK(mtkx_immix_space_mark(&space, head.value, 1));
  CHECK(mtkx_immix_space_marked_bytes(&space) >= MTKX_IMMIX_LINE_BYTES);
  CHECK(!mtkx_immix_space_mark(&space, 1, 1));
  mtkx_immix_space_reset_blocks(&space);
  CHECK(mtkx_immix_space_used_bytes(&space) == 0);
  CHECK(mtkx_immix_space_marked_bytes(&space) == 0);
  mtkx_immix_space_destroy(&space);

  /* guide 6.3: a 300-byte object on a line boundary touches two lines */
  {
    mtkx_immix_space s;
    mtkx_alloc_result object;
    const mtkx_immix_block *block;
    CHECK(mtkx_immix_space_init(&s, MTKX_IMMIX_BLOCK_BYTES * 2) == MTKX_OK);
    object = mtkx_immix_space_allocate(&s, 300, MTKX_IMMIX_LINE_BYTES);
    CHECK(object.ok);
    CHECK(mtkx_space_contains(&s.base, object.value));
    mtkx_immix_space_clear_marks(&s);
    CHECK(mtkx_immix_space_marked_bytes(&s) == 0);
    CHECK(mtkx_immix_space_mark(&s, object.value, 300));
    CHECK(mtkx_immix_space_marked_bytes(&s) == 2 * MTKX_IMMIX_LINE_BYTES);
    block = mtkx_immix_space_block(&s, 0);
    CHECK(block != NULL && mtkx_immix_block_marked_lines(block) == 2);
    CHECK(mtkx_immix_block_remaining_bytes(block) == MTKX_IMMIX_BLOCK_BYTES - 300);
    CHECK(mtkx_immix_space_block(&s, 1000) == NULL);
    mtkx_immix_space_destroy(&s);
  }
  return 0;
}
