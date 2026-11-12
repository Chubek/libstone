/**
 * @file memtkx/barrier/barrier.h
 * @brief Reference-write barrier stages: range filters and slot recording
 *        (C++ `barrier_non_null()`, `barrier_filter_range()`, ...,
 *        `barrier_record()`, `run_write_barrier()`).
 *
 * A barrier pipeline is an array of ::mtkx_barrier_stage values applied
 * to one ::mtkx_slot. Filters drop the slot ("none"); later filters are
 * skipped and sinks (record/discard) see nothing. This is the
 * optional-propagating behaviour the C++ stages approximated with
 * `std::optional<Slot>`; in C any order of stages is valid.
 *
 * @code
 *   mtkx_address field = 0;
 *   mtkx_slot_buffer modified;
 *   mtkx_slot_buffer_init(&modified);
 *   mtkx_address_range source = mtkx_range(mtkx_as_address(&field),
 *                                           mtkx_as_address(&field) + sizeof field);
 *   mtkx_address_range target = mtkx_range(0x1000, 0x3000);
 *
 *   mtkx_run_write_barrier(mtkx_slot_make(&field, 0x2000), source, target, &modified);
 *   // modified.size == 1; the store itself is still the caller's job
 *   mtkx_slot_buffer_destroy(&modified);
 * @endcode
 */
#ifndef MEMTKX_BARRIER_BARRIER_H
#define MEMTKX_BARRIER_BARRIER_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/types.h"

MTKX_BEGIN_DECLS

/** A reference field being written and the object it will refer to. */
typedef struct mtkx_slot {
  mtkx_address *slot_address;
  mtkx_address target_object;
} mtkx_slot;

/** A reference field being read and the loaded object. */
typedef struct mtkx_read_slot {
  const mtkx_address *slot_address;
  mtkx_address loaded_object;
} mtkx_read_slot;

/** Builds a slot. */
MTKX_INLINE mtkx_slot mtkx_slot_make(mtkx_address *slot_address, mtkx_address target) {
  mtkx_slot s;
  s.slot_address = slot_address;
  s.target_object = target;
  return s;
}

/* ------------------------------------------------------------------ */
/*  Slot buffers                                                      */
/* ------------------------------------------------------------------ */

/** A growable record of slots (C++ `std::vector<Slot>`). Not thread-safe. */
typedef struct mtkx_slot_buffer {
  mtkx_slot *data;
  size_t size;
  size_t capacity;
  size_t dropped; /**< records lost because growing the buffer failed */
} mtkx_slot_buffer;

/** Initialises an empty buffer. */
MTKX_API void mtkx_slot_buffer_init(mtkx_slot_buffer *buffer);
/** Releases storage. */
MTKX_API void mtkx_slot_buffer_destroy(mtkx_slot_buffer *buffer);
/** Removes all records (capacity kept, `dropped` reset). */
MTKX_API void mtkx_slot_buffer_clear(mtkx_slot_buffer *buffer);
/** Appends a record. */
MTKX_API mtkx_status mtkx_slot_buffer_push(mtkx_slot_buffer *buffer, mtkx_slot slot);

/* ------------------------------------------------------------------ */
/*  Stages                                                            */
/* ------------------------------------------------------------------ */

/** Stage kinds. */
typedef enum mtkx_barrier_stage_kind {
  MTKX_BARRIER_NON_NULL = 0,            /**< keep slots with a slot address and a nonzero target */
  MTKX_BARRIER_FILTER_RANGE = 1,        /**< keep slots whose *location* lies in `source` */
  MTKX_BARRIER_FILTER_TARGET_RANGE = 2, /**< keep slots whose *target* lies in `target` */
  MTKX_BARRIER_CROSS_REGION = 3,        /**< location in source and target in target */
  MTKX_BARRIER_SATURATING_FILTER = 4,   /**< same predicate as CROSS_REGION */
  MTKX_BARRIER_RECORD = 5,              /**< sink: append surviving slot to `buffer` */
  MTKX_BARRIER_DISCARD = 6,             /**< sink: drop it */
  MTKX_BARRIER_CUSTOM = 7               /**< fn(slot, ctx): may edit the slot; false drops it */
} mtkx_barrier_stage_kind;

/** Application-defined barrier stage. */
typedef bool (*mtkx_barrier_custom_fn)(mtkx_slot *slot, void *ctx);

/** One stage. Build with the constructors below. */
typedef struct mtkx_barrier_stage {
  mtkx_barrier_stage_kind kind;
  mtkx_address_range source;
  mtkx_address_range target;
  mtkx_slot_buffer *buffer;
  mtkx_barrier_custom_fn fn;
  void *ctx;
} mtkx_barrier_stage;

/** @internal */
MTKX_INLINE mtkx_barrier_stage mtkx_barrier_stage_make_(mtkx_barrier_stage_kind kind) {
  mtkx_barrier_stage s;
  s.kind = kind;
  s.source = mtkx_range(0, 0);
  s.target = mtkx_range(0, 0);
  s.buffer = NULL;
  s.fn = NULL;
  s.ctx = NULL;
  return s;
}

/** C++ `barrier_non_null()`. */
MTKX_INLINE mtkx_barrier_stage mtkx_barrier_non_null(void) {
  return mtkx_barrier_stage_make_(MTKX_BARRIER_NON_NULL);
}
/** C++ `barrier_filter_range(range)`: checks the slot location. */
MTKX_INLINE mtkx_barrier_stage mtkx_barrier_filter_range(mtkx_address_range range) {
  mtkx_barrier_stage s = mtkx_barrier_stage_make_(MTKX_BARRIER_FILTER_RANGE);
  s.source = range;
  return s;
}
/** C++ `barrier_filter_target_range(range)`: checks the target. */
MTKX_INLINE mtkx_barrier_stage mtkx_barrier_filter_target_range(mtkx_address_range range) {
  mtkx_barrier_stage s = mtkx_barrier_stage_make_(MTKX_BARRIER_FILTER_TARGET_RANGE);
  s.target = range;
  return s;
}
/** C++ `barrier_cross_region(source, target)`. */
MTKX_INLINE mtkx_barrier_stage mtkx_barrier_cross_region(mtkx_address_range source,
                                                         mtkx_address_range target) {
  mtkx_barrier_stage s = mtkx_barrier_stage_make_(MTKX_BARRIER_CROSS_REGION);
  s.source = source;
  s.target = target;
  return s;
}
/** C++ `barrier_saturating_filter(source, target)`. */
MTKX_INLINE mtkx_barrier_stage mtkx_barrier_saturating_filter(mtkx_address_range source,
                                                              mtkx_address_range target) {
  mtkx_barrier_stage s = mtkx_barrier_cross_region(source, target);
  s.kind = MTKX_BARRIER_SATURATING_FILTER;
  return s;
}
/** C++ `barrier_record(buffer)`. */
MTKX_INLINE mtkx_barrier_stage mtkx_barrier_record(mtkx_slot_buffer *buffer) {
  mtkx_barrier_stage s = mtkx_barrier_stage_make_(MTKX_BARRIER_RECORD);
  s.buffer = buffer;
  return s;
}
/** C++ `barrier_discard()`. */
MTKX_INLINE mtkx_barrier_stage mtkx_barrier_discard(void) {
  return mtkx_barrier_stage_make_(MTKX_BARRIER_DISCARD);
}
/** An application-defined stage. */
MTKX_INLINE mtkx_barrier_stage mtkx_barrier_custom(mtkx_barrier_custom_fn fn, void *ctx) {
  mtkx_barrier_stage s = mtkx_barrier_stage_make_(MTKX_BARRIER_CUSTOM);
  s.fn = fn;
  s.ctx = ctx;
  return s;
}

/**
 * @brief Runs @p slot through @p count stages.
 * @return true when the slot survived every filter (and was recorded by
 *         any record stage), false when a stage dropped it.
 */
MTKX_API bool mtkx_barrier_run(mtkx_slot slot, const mtkx_barrier_stage *stages,
                               size_t count);

/**
 * @brief Records @p slot in @p modified when its location lies in
 *        @p source and its target in @p target (C++ `run_write_barrier`).
 *        Does not perform the store.
 * @return true when the slot was recorded.
 */
MTKX_API bool mtkx_run_write_barrier(mtkx_slot slot, mtkx_address_range source,
                                     mtkx_address_range target,
                                     mtkx_slot_buffer *modified);

MTKX_END_DECLS

#endif /* MEMTKX_BARRIER_BARRIER_H */
