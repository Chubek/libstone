/**
 * @file memtkx/scheduler/work_packet.h
 * @brief Work packets and a FIFO work queue (ring buffer).
 */
#ifndef MEMTKX_SCHEDULER_WORK_PACKET_H
#define MEMTKX_SCHEDULER_WORK_PACKET_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/types.h"

MTKX_BEGIN_DECLS

/** What a packet asks for. */
typedef enum mtkx_work_kind {
  MTKX_WORK_ROOT_SCAN = 0,
  MTKX_WORK_PROCESS_EDGE = 1,
  MTKX_WORK_MARK = 2,
  MTKX_WORK_SWEEP = 3,
  MTKX_WORK_RELEASE = 4,
  MTKX_WORK_USER = 5
} mtkx_work_kind;

/** One unit of collector work. */
typedef struct mtkx_work_packet {
  mtkx_work_kind kind;
  mtkx_address object;
  size_t extra;
} mtkx_work_packet;

/** Builds a packet. */
MTKX_INLINE mtkx_work_packet mtkx_work_packet_make(mtkx_work_kind kind, mtkx_address object,
                                                   size_t extra) {
  mtkx_work_packet p;
  p.kind = kind;
  p.object = object;
  p.extra = extra;
  return p;
}

/** FIFO of packets (C++ `WorkQueue`). Not thread-safe. Fields are private. */
typedef struct mtkx_work_queue {
  mtkx_work_packet *data;
  size_t head;
  size_t size;
  size_t capacity;
} mtkx_work_queue;

/** Initialises an empty queue. */
MTKX_API void mtkx_work_queue_init(mtkx_work_queue *queue);
/** Releases storage. */
MTKX_API void mtkx_work_queue_destroy(mtkx_work_queue *queue);
/** Appends a packet. */
MTKX_API mtkx_status mtkx_work_queue_push(mtkx_work_queue *queue, mtkx_work_packet packet);
/** Removes the oldest packet into *packet; false when empty. */
MTKX_API bool mtkx_work_queue_try_pop(mtkx_work_queue *queue, mtkx_work_packet *packet);
/** @return true when empty. */
MTKX_INLINE bool mtkx_work_queue_empty(const mtkx_work_queue *queue) {
  return queue->size == 0;
}
/** @return the number of queued packets. */
MTKX_INLINE size_t mtkx_work_queue_size(const mtkx_work_queue *queue) {
  return queue->size;
}
/** Removes all packets. */
MTKX_INLINE void mtkx_work_queue_clear(mtkx_work_queue *queue) {
  queue->head = 0;
  queue->size = 0;
}

/** Packet handler for mtkx_work_queue_drain(). */
typedef void (*mtkx_work_fn)(const mtkx_work_packet *packet, void *ctx);

/**
 * @brief Pops and handles packets until the queue is empty, including
 *        packets pushed by @p fn. @return the number processed.
 */
MTKX_API size_t mtkx_work_queue_drain(mtkx_work_queue *queue, mtkx_work_fn fn, void *ctx);

MTKX_END_DECLS

#endif /* MEMTKX_SCHEDULER_WORK_PACKET_H */
