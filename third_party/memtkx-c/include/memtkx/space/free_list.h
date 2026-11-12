/**
 * @file memtkx/space/free_list.h
 * @brief Address-ordered free-list allocation with coalescing, the
 *        FreeListSpace, and the segregated (size-class) variant.
 */
#ifndef MEMTKX_SPACE_FREE_LIST_H
#define MEMTKX_SPACE_FREE_LIST_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/space/space.h"

MTKX_BEGIN_DECLS

/** A free cell `[address, address + size)`. */
typedef struct mtkx_free_cell {
  mtkx_address address;
  size_t size;
} mtkx_free_cell;

/** @return one past the cell's last byte. */
MTKX_INLINE mtkx_address mtkx_free_cell_end(mtkx_free_cell cell) {
  return cell.address + cell.size;
}

/**
 * @brief First-fit free list over a borrowed region (C++ `FreeListAllocator`).
 *
 * Cells are kept sorted by address and coalesced. Zero-initialisation is
 * an empty allocator; call mtkx_free_list_allocator_destroy() to release
 * its cell array. Fields are private.
 */
typedef struct mtkx_free_list_allocator {
  mtkx_address start;
  mtkx_address limit;
  mtkx_free_cell *cells;
  size_t cell_count;
  size_t cell_capacity;
  size_t allocations;
  size_t allocated_bytes;
} mtkx_free_list_allocator;

/** Initialises with one free cell covering `[start, end)`. */
MTKX_API mtkx_status mtkx_free_list_allocator_init(mtkx_free_list_allocator *allocator,
                                                   mtkx_address start, mtkx_address end);

/** Replaces the region, clearing cells and counters. */
MTKX_API mtkx_status mtkx_free_list_allocator_reset(mtkx_free_list_allocator *allocator,
                                                    mtkx_address start, mtkx_address end);

/** Releases the cell array. */
MTKX_API void mtkx_free_list_allocator_destroy(mtkx_free_list_allocator *allocator);

/**
 * @brief First-fit allocation in address order; prefix padding and the
 *        remaining suffix stay free.
 * @return MTKX_ALLOC_ALLOCATION_FAILED (zero bytes or bookkeeping OOM),
 *         MTKX_ALLOC_MISALIGNED or MTKX_ALLOC_OUT_OF_MEMORY on failure.
 */
MTKX_API mtkx_alloc_result mtkx_free_list_allocator_allocate(
    mtkx_free_list_allocator *allocator, size_t bytes, size_t alignment);

/**
 * @brief Returns `[address, address + bytes)` to the free list. Bounds are
 *        checked; double frees and wrong sizes are not detected.
 */
MTKX_API bool mtkx_free_list_allocator_free(mtkx_free_list_allocator *allocator,
                                            mtkx_address address, size_t bytes);

/** @return the region start. */
MTKX_INLINE mtkx_address mtkx_free_list_allocator_start(const mtkx_free_list_allocator *a) {
  return a->start;
}
/** @return the region limit. */
MTKX_INLINE mtkx_address mtkx_free_list_allocator_limit(const mtkx_free_list_allocator *a) {
  return a->limit;
}
/** @return true when @p address lies in the region. */
MTKX_INLINE bool mtkx_free_list_allocator_contains(const mtkx_free_list_allocator *a,
                                                   mtkx_address address) {
  return address >= a->start && address < a->limit;
}
/** @return the number of free cells. */
MTKX_INLINE size_t mtkx_free_list_allocator_free_cells(const mtkx_free_list_allocator *a) {
  return a->cell_count;
}
/** @return the sum of free cell sizes. */
MTKX_API size_t mtkx_free_list_allocator_free_bytes(const mtkx_free_list_allocator *a);
/** @return successful allocations since the last reset. */
MTKX_INLINE size_t mtkx_free_list_allocator_allocation_count(
    const mtkx_free_list_allocator *a) {
  return a->allocations;
}
/** @return requested bytes allocated since the last reset. */
MTKX_INLINE size_t mtkx_free_list_allocator_allocated_bytes(
    const mtkx_free_list_allocator *a) {
  return a->allocated_bytes;
}

/** A free list with space metadata (C++ `FreeListSpace`). */
typedef struct mtkx_free_list_space {
  mtkx_space base;
  mtkx_free_list_allocator allocator;
} mtkx_free_list_space;

/** Initialises a free-list space over `[start, end)`. */
MTKX_API mtkx_status mtkx_free_list_space_init(mtkx_free_list_space *space,
                                               const char *name, mtkx_address start,
                                               mtkx_address end);

/** Allocates (equivalent to mtkx_space_allocate(&space->base, ...)). */
MTKX_API mtkx_alloc_result mtkx_free_list_space_allocate(mtkx_free_list_space *space,
                                                         size_t bytes, size_t alignment);

/** Frees and records the deallocation. */
MTKX_API bool mtkx_free_list_space_deallocate(mtkx_free_list_space *space,
                                              mtkx_address address, size_t bytes);

/** Resets the allocator to the whole region; metrics are kept. */
MTKX_API mtkx_status mtkx_free_list_space_reset(mtkx_free_list_space *space);

/** Releases the space. */
MTKX_API void mtkx_free_list_space_destroy(mtkx_free_list_space *space);

/* ------------------------------------------------------------------ */
/*  Segregated free lists                                             */
/* ------------------------------------------------------------------ */

/** Number of size classes. */
#define MTKX_SEGREGATED_CLASS_COUNT 8

/** The class thresholds: 16, 32, 64, 128, 256, 512, 1024, 4096. */
MTKX_API extern const size_t mtkx_segregated_size_classes[MTKX_SEGREGATED_CLASS_COUNT];

/**
 * @brief Eight independent free lists over equal partitions of a region
 *        (the last takes the remainder) (C++ `SegregatedFreeListAllocator`).
 */
typedef struct mtkx_segregated_allocator {
  mtkx_address start;
  mtkx_address limit;
  mtkx_free_list_allocator bins[MTKX_SEGREGATED_CLASS_COUNT];
  mtkx_address_range ranges[MTKX_SEGREGATED_CLASS_COUNT];
} mtkx_segregated_allocator;

/** Initialises over `[start, end)`. */
MTKX_API mtkx_status mtkx_segregated_allocator_init(mtkx_segregated_allocator *allocator,
                                                    mtkx_address start, mtkx_address end);

/** Re-partitions over `[start, end)`. */
MTKX_API mtkx_status mtkx_segregated_allocator_reset(mtkx_segregated_allocator *allocator,
                                                     mtkx_address start, mtkx_address end);

/** Releases the bins. */
MTKX_API void mtkx_segregated_allocator_destroy(mtkx_segregated_allocator *allocator);

/** Tries the first class >= @p bytes, then larger classes. */
MTKX_API mtkx_alloc_result mtkx_segregated_allocator_allocate(
    mtkx_segregated_allocator *allocator, size_t bytes, size_t alignment);

/** Frees into the partition containing @p address. */
MTKX_API bool mtkx_segregated_allocator_free(mtkx_segregated_allocator *allocator,
                                             mtkx_address address, size_t bytes);

/** @return the region start. */
MTKX_INLINE mtkx_address mtkx_segregated_allocator_start(const mtkx_segregated_allocator *a) {
  return a->start;
}
/** @return the region limit. */
MTKX_INLINE mtkx_address mtkx_segregated_allocator_limit(const mtkx_segregated_allocator *a) {
  return a->limit;
}
/** @return MTKX_SEGREGATED_CLASS_COUNT. */
MTKX_INLINE size_t mtkx_segregated_allocator_class_count(const mtkx_segregated_allocator *a) {
  (void)a;
  return MTKX_SEGREGATED_CLASS_COUNT;
}

MTKX_END_DECLS

#endif /* MEMTKX_SPACE_FREE_LIST_H */
