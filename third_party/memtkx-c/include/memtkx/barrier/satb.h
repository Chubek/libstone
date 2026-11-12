/**
 * @file memtkx/barrier/satb.h
 * @brief Snapshot-at-the-beginning queue and barrier.
 */
#ifndef MEMTKX_BARRIER_SATB_H
#define MEMTKX_BARRIER_SATB_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/types.h"
#include "memtkx/core/vec.h"

MTKX_BEGIN_DECLS

/** Queue of overwritten references (C++ `SatbQueue`). Not thread-safe. */
typedef struct mtkx_satb_queue {
  mtkx_address_vec entries;
  size_t dropped; /**< entries lost because growing the queue failed */
} mtkx_satb_queue;

/** Initialises an empty queue. */
MTKX_API void mtkx_satb_queue_init(mtkx_satb_queue *queue);
/** Releases storage. */
MTKX_API void mtkx_satb_queue_destroy(mtkx_satb_queue *queue);
/** Appends @p object. */
MTKX_API mtkx_status mtkx_satb_queue_push(mtkx_satb_queue *queue, mtkx_address object);
/**
 * @brief Moves all entries into @p out (whose previous contents are
 *        discarded) and leaves the queue empty (C++ `take_entries`).
 */
MTKX_API void mtkx_satb_queue_take_entries(mtkx_satb_queue *queue, mtkx_address_vec *out);
/** @return true when empty. */
MTKX_INLINE bool mtkx_satb_queue_empty(const mtkx_satb_queue *queue) {
  return queue->entries.size == 0;
}
/** @return the number of entries. */
MTKX_INLINE size_t mtkx_satb_queue_size(const mtkx_satb_queue *queue) {
  return queue->entries.size;
}
/** Removes all entries. */
MTKX_INLINE void mtkx_satb_queue_clear(mtkx_satb_queue *queue) {
  mtkx_address_vec_clear(&queue->entries);
}

/** Barrier capturing old references into a queue (C++ `SatbBarrier`). */
typedef struct mtkx_satb_barrier {
  mtkx_satb_queue *queue;
} mtkx_satb_barrier;

/** Binds a barrier to @p queue. */
MTKX_INLINE mtkx_satb_barrier mtkx_satb_barrier_make(mtkx_satb_queue *queue) {
  mtkx_satb_barrier b;
  b.queue = queue;
  return b;
}

/** Reads *slot_address, queues it when nonzero, and returns it (0 for NULL). */
MTKX_API mtkx_address mtkx_satb_barrier_capture(const mtkx_satb_barrier *barrier,
                                                mtkx_address *slot_address);

/** Captures the old value, then stores @p new_value into the slot. */
MTKX_API void mtkx_satb_barrier_write(const mtkx_satb_barrier *barrier,
                                      mtkx_address *slot_address, mtkx_address new_value);

MTKX_END_DECLS

#endif /* MEMTKX_BARRIER_SATB_H */
