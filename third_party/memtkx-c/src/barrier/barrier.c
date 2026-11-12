#include "memtkx/barrier/barrier.h"
#include "memtkx/barrier/card_table.h"
#include "memtkx/barrier/satb.h"

#include <string.h>

#include "internal/common.h"

/* ------------------------------------------------------------------ */
/*  Slot buffers and pipelines                                        */
/* ------------------------------------------------------------------ */

void mtkx_slot_buffer_init(mtkx_slot_buffer *buffer) { memset(buffer, 0, sizeof *buffer); }

void mtkx_slot_buffer_destroy(mtkx_slot_buffer *buffer) {
  mtkx_free(buffer->data);
  mtkx_slot_buffer_init(buffer);
}

void mtkx_slot_buffer_clear(mtkx_slot_buffer *buffer) {
  buffer->size = 0;
  buffer->dropped = 0;
}

mtkx_status mtkx_slot_buffer_push(mtkx_slot_buffer *buffer, mtkx_slot slot) {
  if (buffer->size == buffer->capacity &&
      MTKX__GROW(buffer->data, buffer->capacity, buffer->size + 1) != MTKX_OK) {
    ++buffer->dropped;
    return MTKX_ERR_NOMEM;
  }
  buffer->data[buffer->size++] = slot;
  return MTKX_OK;
}

static bool mtkx__slot_in(mtkx_address_range range, const mtkx_slot *slot) {
  return mtkx_range_contains(range, mtkx_as_address(slot->slot_address));
}

bool mtkx_barrier_run(mtkx_slot slot, const mtkx_barrier_stage *stages, size_t count) {
  size_t i;
  for (i = 0; i < count; ++i) {
    const mtkx_barrier_stage *s = &stages[i];
    bool keep = true;
    switch (s->kind) {
      case MTKX_BARRIER_NON_NULL:
        keep = slot.slot_address != NULL && slot.target_object != 0;
        break;
      case MTKX_BARRIER_FILTER_RANGE:
        keep = mtkx__slot_in(s->source, &slot);
        break;
      case MTKX_BARRIER_FILTER_TARGET_RANGE:
        keep = mtkx_range_contains(s->target, slot.target_object);
        break;
      case MTKX_BARRIER_CROSS_REGION:
      case MTKX_BARRIER_SATURATING_FILTER:
        keep = mtkx__slot_in(s->source, &slot) &&
               mtkx_range_contains(s->target, slot.target_object);
        break;
      case MTKX_BARRIER_RECORD:
        keep = s->buffer != NULL && mtkx_slot_buffer_push(s->buffer, slot) == MTKX_OK;
        break;
      case MTKX_BARRIER_DISCARD:
        keep = false;
        break;
      case MTKX_BARRIER_CUSTOM:
        keep = s->fn == NULL || s->fn(&slot, s->ctx);
        break;
    }
    if (!keep) {
      return false;
    }
  }
  return true;
}

bool mtkx_run_write_barrier(mtkx_slot slot, mtkx_address_range source,
                            mtkx_address_range target, mtkx_slot_buffer *modified) {
  mtkx_barrier_stage stages[2];
  stages[0] = mtkx_barrier_saturating_filter(source, target);
  stages[1] = mtkx_barrier_record(modified);
  return mtkx_barrier_run(slot, stages, 2);
}

/* ------------------------------------------------------------------ */
/*  Card tables                                                       */
/* ------------------------------------------------------------------ */

mtkx_status mtkx_card_table_init(mtkx_card_table *table, mtkx_address base,
                                 size_t region_bytes, size_t card_bytes) {
  table->base = base;
  table->region_bytes = region_bytes;
  table->card_bytes = card_bytes;
  table->cards = NULL;
  table->count = card_bytes == 0 ? 0u : region_bytes / card_bytes +
                                            (region_bytes % card_bytes != 0 ? 1u : 0u);
  if (table->count != 0) {
    table->cards = (uint8_t *)mtkx_calloc(table->count, 1);
    if (table->cards == NULL) {
      table->count = 0;
      return MTKX_ERR_NOMEM;
    }
  }
  return MTKX_OK;
}

void mtkx_card_table_destroy(mtkx_card_table *table) {
  mtkx_free(table->cards);
  table->cards = NULL;
  table->count = 0;
}

size_t mtkx_card_table_card_index(const mtkx_card_table *table, mtkx_address address) {
  if (table->card_bytes == 0 || table->count == 0) {
    return table->count;
  }
  if (address < table->base) {
    return table->count;
  }
  if (address - table->base >= table->region_bytes) {
    return table->count;
  }
  return (size_t)(address - table->base) / table->card_bytes;
}

void mtkx_card_table_mark(mtkx_card_table *table, mtkx_address address) {
  size_t index = mtkx_card_table_card_index(table, address);
  if (index < table->count) {
    table->cards[index] = 1;
  }
}

bool mtkx_card_table_is_marked(const mtkx_card_table *table, mtkx_address address) {
  size_t index = mtkx_card_table_card_index(table, address);
  return index < table->count && table->cards[index] != 0;
}

void mtkx_card_table_clear(mtkx_card_table *table, mtkx_address address) {
  size_t index = mtkx_card_table_card_index(table, address);
  if (index < table->count) {
    table->cards[index] = 0;
  }
}

void mtkx_card_table_clear_all(mtkx_card_table *table) {
  if (table->count != 0) {
    memset(table->cards, 0, table->count);
  }
}

size_t mtkx_card_table_marked_cards(const mtkx_card_table *table) {
  size_t total = 0;
  size_t i;
  for (i = 0; i < table->count; ++i) {
    total += table->cards[i] != 0 ? 1u : 0u;
  }
  return total;
}

void mtkx_card_table_barrier_apply(const mtkx_card_table_barrier *barrier,
                                   mtkx_address *slot_address, mtkx_address new_value) {
  MTKX__UNUSED(new_value);
  if (slot_address != NULL && barrier->table != NULL) {
    mtkx_card_table_mark(barrier->table, mtkx_as_address(slot_address));
  }
}

/* ------------------------------------------------------------------ */
/*  SATB                                                              */
/* ------------------------------------------------------------------ */

void mtkx_satb_queue_init(mtkx_satb_queue *queue) {
  mtkx_address_vec_init(&queue->entries);
  queue->dropped = 0;
}

void mtkx_satb_queue_destroy(mtkx_satb_queue *queue) {
  mtkx_address_vec_destroy(&queue->entries);
  queue->dropped = 0;
}

mtkx_status mtkx_satb_queue_push(mtkx_satb_queue *queue, mtkx_address object) {
  mtkx_status status = mtkx_address_vec_push(&queue->entries, object);
  if (status != MTKX_OK) {
    ++queue->dropped;
  }
  return status;
}

void mtkx_satb_queue_take_entries(mtkx_satb_queue *queue, mtkx_address_vec *out) {
  mtkx_address_vec_swap(&queue->entries, out);
  mtkx_address_vec_clear(&queue->entries);
}

mtkx_address mtkx_satb_barrier_capture(const mtkx_satb_barrier *barrier,
                                       mtkx_address *slot_address) {
  mtkx_address old_value;
  if (slot_address == NULL) {
    return 0;
  }
  old_value = *slot_address;
  if (old_value != 0 && barrier->queue != NULL) {
    (void)mtkx_satb_queue_push(barrier->queue, old_value);
  }
  return old_value;
}

void mtkx_satb_barrier_write(const mtkx_satb_barrier *barrier, mtkx_address *slot_address,
                             mtkx_address new_value) {
  if (slot_address == NULL) {
    return;
  }
  (void)mtkx_satb_barrier_capture(barrier, slot_address);
  *slot_address = new_value;
}
