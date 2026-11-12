/* Port of tests/test_barrier.cpp plus stage-composition coverage. */
#include <memtkx/memtkx.h>

#include "test_support.h"

static bool rewrite_target(mtkx_slot *slot, void *ctx) {
  slot->target_object = *(const mtkx_address *)ctx;
  return true;
}

int main(void) {
  mtkx_address object = 0;
  mtkx_slot slot = mtkx_slot_make(&object, 0x2000);
  mtkx_address_range source =
      mtkx_range(mtkx_as_address(&object), mtkx_as_address(&object) + 1);
  mtkx_address_range target = mtkx_range(0x1000, 0x3000);
  mtkx_slot_buffer buffer;

  mtkx_slot_buffer_init(&buffer);
  CHECK(mtkx_run_write_barrier(slot, source, target, &buffer));
  CHECK(buffer.size == 1);
  CHECK(buffer.data[0].target_object == slot.target_object);
  CHECK(object == 0); /* recording does not store */

  mtkx_slot_buffer_clear(&buffer);
  CHECK(!mtkx_run_write_barrier(mtkx_slot_make(NULL, 0x2000), source, target, &buffer));
  CHECK(buffer.size == 0);

  /* Arbitrary stage chains: filters short-circuit, sinks see "none". */
  {
    mtkx_address replacement = 0x2800;
    mtkx_barrier_stage chain[5];
    chain[0] = mtkx_barrier_non_null();
    chain[1] = mtkx_barrier_filter_range(source);
    chain[2] = mtkx_barrier_custom(rewrite_target, &replacement);
    chain[3] = mtkx_barrier_filter_target_range(target);
    chain[4] = mtkx_barrier_record(&buffer);
    CHECK(mtkx_barrier_run(slot, chain, 5));
    CHECK(buffer.size == 1 && buffer.data[0].target_object == 0x2800);
    CHECK(!mtkx_barrier_run(mtkx_slot_make(&object, 0), chain, 5));
    chain[4] = mtkx_barrier_discard();
    CHECK(!mtkx_barrier_run(slot, chain, 5));
    CHECK(buffer.size == 1);
    chain[1] = mtkx_barrier_cross_region(source, mtkx_range(0, 0x10));
    chain[4] = mtkx_barrier_record(&buffer);
    CHECK(!mtkx_barrier_run(slot, chain, 5));
    CHECK(buffer.size == 1);
  }
  mtkx_slot_buffer_destroy(&buffer);

  /* --- card tables --- */
  {
    mtkx_card_table cards, near_zero, zero_card;
    CHECK(mtkx_card_table_init(&cards, 0, 4096, 512) == MTKX_OK);
    CHECK(mtkx_card_table_card_count(&cards) == 8);
    mtkx_card_table_mark(&cards, 513);
    CHECK(mtkx_card_table_is_marked(&cards, 513));
    CHECK(mtkx_card_table_marked_cards(&cards) == 1);
    mtkx_card_table_clear(&cards, 513);
    CHECK(!mtkx_card_table_is_marked(&cards, 513));
    mtkx_card_table_mark(&cards, 1);
    mtkx_card_table_mark(&cards, 4000);
    CHECK(mtkx_card_table_marked_cards(&cards) == 2);
    mtkx_card_table_clear_all(&cards);
    CHECK(mtkx_card_table_marked_cards(&cards) == 0);
    mtkx_card_table_destroy(&cards);

    CHECK(mtkx_card_table_init(&near_zero, 0, 4096, 512) == MTKX_OK);
    mtkx_card_table_mark(&near_zero, 1024);
    CHECK(mtkx_card_table_is_marked(&near_zero, 1024));
    mtkx_card_table_mark(&near_zero, 4096 + 1);
    CHECK(!mtkx_card_table_is_marked(&near_zero, 4096 + 1));
    mtkx_card_table_clear(&near_zero, 1024);
    CHECK(!mtkx_card_table_is_marked(&near_zero, 1024));
    mtkx_card_table_mark(&near_zero, mtkx_card_table_base(&near_zero) +
                                         mtkx_card_table_card_bytes(&near_zero));
    CHECK(mtkx_card_table_is_marked(&near_zero, mtkx_card_table_base(&near_zero) +
                                                    mtkx_card_table_card_bytes(&near_zero)));
    mtkx_card_table_destroy(&near_zero);

    CHECK(mtkx_card_table_init(&zero_card, 0, 4096, 0) == MTKX_OK);
    CHECK(mtkx_card_table_card_count(&zero_card) == 0);
    mtkx_card_table_mark(&zero_card, 100);
    CHECK(!mtkx_card_table_is_marked(&zero_card, 100));
    mtkx_card_table_destroy(&zero_card);
  }

  /* --- SATB and the card barrier (guide chapter 9 example) --- */
  {
    mtkx_satb_queue queue;
    mtkx_satb_barrier satb;
    mtkx_address old_value = 77;
    mtkx_address slot_value = old_value;
    mtkx_address_vec captured = MTKX_ADDRESS_VEC_INIT;
    mtkx_card_table cards;
    mtkx_card_table_barrier card_barrier;
    mtkx_address field = 77;

    mtkx_satb_queue_init(&queue);
    satb = mtkx_satb_barrier_make(&queue);
    CHECK(mtkx_satb_barrier_capture(&satb, &slot_value) == old_value);
    CHECK(mtkx_satb_queue_size(&queue) == 1);
    mtkx_satb_barrier_write(&satb, &slot_value, 99);
    CHECK(slot_value == 99);
    CHECK(mtkx_satb_queue_size(&queue) == 2);
    CHECK(mtkx_satb_barrier_capture(&satb, NULL) == 0);
    mtkx_satb_queue_take_entries(&queue, &captured);
    CHECK(captured.size == 2 && captured.data[0] == 77 && captured.data[1] == 77);
    CHECK(mtkx_satb_queue_empty(&queue));
    mtkx_address_vec_destroy(&captured);
    mtkx_satb_queue_destroy(&queue);

    CHECK(mtkx_card_table_init(&cards, mtkx_as_address(&field), sizeof field,
                               MTKX_DEFAULT_CARD_BYTES) == MTKX_OK);
    card_barrier = mtkx_card_table_barrier_make(&cards);
    mtkx_card_table_barrier_apply(&card_barrier, &field, 88);
    CHECK(field == 77 && mtkx_card_table_is_marked(&cards, mtkx_as_address(&field)));
    mtkx_card_table_destroy(&cards);
  }
  return 0;
}
