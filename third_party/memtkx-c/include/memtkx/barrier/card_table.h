/**
 * @file memtkx/barrier/card_table.h
 * @brief Card tables and the card-marking barrier.
 */
#ifndef MEMTKX_BARRIER_CARD_TABLE_H
#define MEMTKX_BARRIER_CARD_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "memtkx/core/base.h"
#include "memtkx/core/types.h"

MTKX_BEGIN_DECLS

/** Default card size (512 bytes). */
#define MTKX_DEFAULT_CARD_BYTES ((size_t)512)

/**
 * @brief One byte per card over `[base, base + region_bytes)`
 *        (C++ `CardTable`). Not atomic; not thread-safe.
 */
typedef struct mtkx_card_table {
  mtkx_address base;
  size_t region_bytes;
  size_t card_bytes;
  uint8_t *cards;
  size_t count;
} mtkx_card_table;

/**
 * @brief Initialises the table. A zero @p card_bytes creates a table with
 *        no cards. Pass MTKX_DEFAULT_CARD_BYTES for the C++ default.
 */
MTKX_API mtkx_status mtkx_card_table_init(mtkx_card_table *table, mtkx_address base,
                                          size_t region_bytes, size_t card_bytes);

/** Releases the cards. */
MTKX_API void mtkx_card_table_destroy(mtkx_card_table *table);

/** @return the card index of @p address, or card_count() when outside. */
MTKX_API size_t mtkx_card_table_card_index(const mtkx_card_table *table,
                                           mtkx_address address);
/** Dirties the card of @p address (ignored when outside). */
MTKX_API void mtkx_card_table_mark(mtkx_card_table *table, mtkx_address address);
/** @return whether the card of @p address is dirty. */
MTKX_API bool mtkx_card_table_is_marked(const mtkx_card_table *table, mtkx_address address);
/** Cleans the card of @p address. */
MTKX_API void mtkx_card_table_clear(mtkx_card_table *table, mtkx_address address);
/** Cleans every card. */
MTKX_API void mtkx_card_table_clear_all(mtkx_card_table *table);
/** @return the number of dirty cards. */
MTKX_API size_t mtkx_card_table_marked_cards(const mtkx_card_table *table);
/** @return the number of cards. */
MTKX_INLINE size_t mtkx_card_table_card_count(const mtkx_card_table *table) {
  return table->count;
}
/** @return the card size. */
MTKX_INLINE size_t mtkx_card_table_card_bytes(const mtkx_card_table *table) {
  return table->card_bytes;
}
/** @return the region base. */
MTKX_INLINE mtkx_address mtkx_card_table_base(const mtkx_card_table *table) {
  return table->base;
}

/** Card-marking barrier bound to a table (C++ `CardTableBarrier`). */
typedef struct mtkx_card_table_barrier {
  mtkx_card_table *table;
} mtkx_card_table_barrier;

/** Binds a barrier to @p table. */
MTKX_INLINE mtkx_card_table_barrier mtkx_card_table_barrier_make(mtkx_card_table *table) {
  mtkx_card_table_barrier b;
  b.table = table;
  return b;
}

/**
 * @brief Marks the card of @p slot_address; @p new_value is ignored and the
 *        field is *not* written (C++ `operator()(slot, value)`).
 */
MTKX_API void mtkx_card_table_barrier_apply(const mtkx_card_table_barrier *barrier,
                                            mtkx_address *slot_address,
                                            mtkx_address new_value);

MTKX_END_DECLS

#endif /* MEMTKX_BARRIER_CARD_TABLE_H */
