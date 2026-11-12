/**
 * @file memtkx/space/bump_pointer.h
 * @brief Bump-pointer allocation over borrowed storage and the BumpSpace.
 */
#ifndef MEMTKX_SPACE_BUMP_POINTER_H
#define MEMTKX_SPACE_BUMP_POINTER_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/space/space.h"

MTKX_BEGIN_DECLS

/**
 * @brief Linear allocator over a borrowed region (C++ `BumpPointerAllocator`).
 *
 * Plain value type; zero-initialisation gives an empty allocator. Fields
 * are private; use the accessors.
 */
typedef struct mtkx_bump_allocator {
  mtkx_address start;
  mtkx_address limit;
  mtkx_address cursor;
  size_t allocations;
  size_t allocated_bytes;
} mtkx_bump_allocator;

/**
 * @brief Initialises over `[start, end)`, aligning start up and end down
 *        to MTKX_MAX_ALIGN. Counters start at zero.
 */
MTKX_API void mtkx_bump_allocator_init(mtkx_bump_allocator *allocator, mtkx_address start,
                                       mtkx_address end);

/**
 * @brief Allocates @p bytes aligned to @p alignment.
 * @return MTKX_ALLOC_ALLOCATION_FAILED for zero bytes,
 *         MTKX_ALLOC_MISALIGNED for a non-power-of-two alignment,
 *         MTKX_ALLOC_OUT_OF_MEMORY when the request does not fit.
 */
MTKX_API mtkx_alloc_result mtkx_bump_allocator_allocate(mtkx_bump_allocator *allocator,
                                                        size_t bytes, size_t alignment);

/**
 * @brief Inline fast path: allocates exactly like
 *        mtkx_bump_allocator_allocate() but returns the address, or 0 on
 *        any failure. Call the full function when the error reason matters.
 */
MTKX_INLINE mtkx_address mtkx_bump_allocator_try_allocate(mtkx_bump_allocator *allocator,
                                                          size_t bytes, size_t alignment) {
  mtkx_address aligned;
  if (bytes == 0 || !mtkx_is_power_of_two(alignment)) {
    return 0;
  }
  aligned = mtkx_align_up(allocator->cursor, alignment);
  if (aligned < allocator->cursor || aligned >= allocator->limit ||
      (size_t)(allocator->limit - aligned) < bytes) {
    return 0;
  }
  allocator->cursor = aligned + bytes;
  allocator->allocations += 1;
  allocator->allocated_bytes += bytes;
  return aligned;
}

/** Returns the cursor to the start. Counters are kept; memory is untouched. */
MTKX_INLINE void mtkx_bump_allocator_reset(mtkx_bump_allocator *allocator) {
  allocator->cursor = allocator->start;
}

/** Replaces the region (aligned as in init) and resets the cursor. Counters are kept. */
MTKX_API void mtkx_bump_allocator_reset_region(mtkx_bump_allocator *allocator,
                                               mtkx_address start, mtkx_address end);

/** @return the aligned start. */
MTKX_INLINE mtkx_address mtkx_bump_allocator_start(const mtkx_bump_allocator *a) {
  return a->start;
}
/** @return the aligned limit. */
MTKX_INLINE mtkx_address mtkx_bump_allocator_limit(const mtkx_bump_allocator *a) {
  return a->limit;
}
/** @return the cursor. */
MTKX_INLINE mtkx_address mtkx_bump_allocator_cursor(const mtkx_bump_allocator *a) {
  return a->cursor;
}
/** @return bytes after the cursor. */
MTKX_INLINE size_t mtkx_bump_allocator_remaining(const mtkx_bump_allocator *a) {
  return a->cursor >= a->limit ? 0u : (size_t)(a->limit - a->cursor);
}
/** @return cursor distance from start, including padding. */
MTKX_INLINE size_t mtkx_bump_allocator_used_bytes(const mtkx_bump_allocator *a) {
  return a->cursor >= a->start ? (size_t)(a->cursor - a->start) : 0u;
}
/** @return the number of successful requests. */
MTKX_INLINE size_t mtkx_bump_allocator_allocation_count(const mtkx_bump_allocator *a) {
  return a->allocations;
}
/** @return the sum of successful requested sizes. */
MTKX_INLINE size_t mtkx_bump_allocator_allocated_bytes(const mtkx_bump_allocator *a) {
  return a->allocated_bytes;
}

/**
 * @brief A bump allocator with space metadata (C++ `BumpSpace`).
 *        `base` is first, so `&space->base` works as an `mtkx_space *`.
 */
typedef struct mtkx_bump_space {
  mtkx_space base;
  mtkx_bump_allocator allocator;
} mtkx_bump_space;

/** Initialises a bump space named @p name over `[start, end)`. */
MTKX_API mtkx_status mtkx_bump_space_init(mtkx_bump_space *space, const char *name,
                                          mtkx_address start, mtkx_address end);

/** Allocates (equivalent to mtkx_space_allocate(&space->base, ...)). */
MTKX_API mtkx_alloc_result mtkx_bump_space_allocate(mtkx_bump_space *space, size_t bytes,
                                                    size_t alignment);

/** Resets the cursor; metrics are kept. */
MTKX_INLINE void mtkx_bump_space_reset(mtkx_bump_space *space) {
  mtkx_bump_allocator_reset(&space->allocator);
}

/** Releases the space's name. */
MTKX_API void mtkx_bump_space_destroy(mtkx_bump_space *space);

MTKX_END_DECLS

#endif /* MEMTKX_SPACE_BUMP_POINTER_H */
