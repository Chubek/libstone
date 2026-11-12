/**
 * @file memtkx/space/space.h
 * @brief The space base type: descriptor, metrics and an allocation vtable
 *        (the C counterpart of the CRTP `SpaceBase<Derived>`).
 *
 * A concrete space embeds ::mtkx_space as its **first member** and supplies
 * an ::mtkx_space_ops table. mtkx_space_allocate() validates the alignment
 * and dispatches to `ops->allocate_impl`, exactly like
 * `SpaceBase::allocate` dispatched to `Derived::allocate_impl`. Unlike the
 * C++ version, every space can be used through an `mtkx_space *`.
 *
 * @code
 *   typedef struct quota_space {
 *     mtkx_space base;              // must be first
 *     mtkx_bump_allocator bump;
 *     size_t quota, charged;
 *   } quota_space;
 *
 *   static mtkx_alloc_result quota_allocate(mtkx_space *s, size_t n, size_t a) {
 *     quota_space *q = (quota_space *)s;
 *     ...
 *     mtkx_space_record_allocation(s, n);
 *   }
 *   static const mtkx_space_ops quota_ops = { quota_allocate, NULL };
 * @endcode
 */
#ifndef MEMTKX_SPACE_SPACE_H
#define MEMTKX_SPACE_SPACE_H

#include <stddef.h>
#include <stdint.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"

MTKX_BEGIN_DECLS

/** Allocation mechanism categories. */
typedef enum mtkx_space_kind {
  MTKX_SPACE_BUMP_POINTER = 0,
  MTKX_SPACE_FREE_LIST = 1,
  MTKX_SPACE_IMMIX = 2,
  MTKX_SPACE_LARGE_OBJECT = 3,
  MTKX_SPACE_NURSERY = 4,
  MTKX_SPACE_SEMI_SPACE = 5
} mtkx_space_kind;

/** @return "bump-pointer", "free-list", "immix", "large-object", "nursery" or "semi-space". */
MTKX_API const char *mtkx_space_kind_string(mtkx_space_kind kind);

/** Cumulative space accounting (requested bytes; padding excluded). */
typedef struct mtkx_space_metrics {
  size_t allocated_bytes;
  size_t live_bytes;
  size_t total_allocations;
  size_t total_deallocations;
  size_t collection_count;
} mtkx_space_metrics;

/** Zeroes every counter. */
MTKX_INLINE void mtkx_space_metrics_reset(mtkx_space_metrics *metrics) {
  metrics->allocated_bytes = 0;
  metrics->live_bytes = 0;
  metrics->total_allocations = 0;
  metrics->total_deallocations = 0;
  metrics->collection_count = 0;
}

/** Identity of a space. The name is an owned copy. */
typedef struct mtkx_space_descriptor {
  char *name;
  mtkx_space_kind kind;
  mtkx_address_range region;
  size_t default_alignment; /**< descriptive; allocate still takes an alignment */
} mtkx_space_descriptor;

typedef struct mtkx_space mtkx_space;

/** Behaviour supplied by a concrete space. */
typedef struct mtkx_space_ops {
  /**
   * Allocates @p bytes at @p alignment (already validated as a power of
   * two). Call mtkx_space_record_allocation() on success.
   */
  mtkx_alloc_result (*allocate_impl)(mtkx_space *space, size_t bytes, size_t alignment);
  /** Releases resources of the concrete space (may be NULL). */
  void (*destroy)(mtkx_space *space);
} mtkx_space_ops;

/** The base of every space. Embed it as the first member. */
struct mtkx_space {
  const mtkx_space_ops *ops;
  mtkx_space_descriptor descriptor;
  mtkx_space_metrics metrics;
};

/**
 * @brief Initialises the base (copies @p name; metrics start at zero).
 * @return MTKX_OK or MTKX_ERR_NOMEM.
 */
MTKX_API mtkx_status mtkx_space_init(mtkx_space *space, const mtkx_space_ops *ops,
                                     const char *name, mtkx_space_kind kind,
                                     mtkx_address_range region, size_t default_alignment);

/** Calls `ops->destroy` (if any) and releases the base's name. */
MTKX_API void mtkx_space_destroy(mtkx_space *space);

/**
 * @brief Allocates through the space (C++ `SpaceBase::allocate`).
 * @return MTKX_ALLOC_MISALIGNED for a non-power-of-two alignment,
 *         otherwise the result of `ops->allocate_impl`.
 */
MTKX_API mtkx_alloc_result mtkx_space_allocate(mtkx_space *space, size_t bytes,
                                               size_t alignment);

/** @return the space's name. */
MTKX_INLINE const char *mtkx_space_name(const mtkx_space *space) {
  return space->descriptor.name != NULL ? space->descriptor.name : "";
}
/** @return the space's kind. */
MTKX_INLINE mtkx_space_kind mtkx_space_get_kind(const mtkx_space *space) {
  return space->descriptor.kind;
}
/** @return the space's region. */
MTKX_INLINE mtkx_address_range mtkx_space_region(const mtkx_space *space) {
  return space->descriptor.region;
}
/** @return the descriptive default alignment. */
MTKX_INLINE size_t mtkx_space_default_alignment(const mtkx_space *space) {
  return space->descriptor.default_alignment;
}
/** @return the space's metrics. */
MTKX_INLINE const mtkx_space_metrics *mtkx_space_metrics_of(const mtkx_space *space) {
  return &space->metrics;
}
/** @return true when @p address lies in the space's region. */
MTKX_INLINE bool mtkx_space_contains(const mtkx_space *space, mtkx_address address) {
  return mtkx_range_contains(space->descriptor.region, address);
}

/** Records a successful allocation of @p bytes (for implementations). */
MTKX_INLINE void mtkx_space_record_allocation(mtkx_space *space, size_t bytes) {
  space->metrics.allocated_bytes += bytes;
  space->metrics.live_bytes += bytes;
  ++space->metrics.total_allocations;
}

/** Records a deallocation of @p bytes (for implementations). */
MTKX_INLINE void mtkx_space_record_deallocation(mtkx_space *space, size_t bytes) {
  if (bytes <= space->metrics.live_bytes) {
    space->metrics.live_bytes -= bytes;
  }
  ++space->metrics.total_deallocations;
}

/** Records a collection (for implementations). */
MTKX_INLINE void mtkx_space_record_collection(mtkx_space *space) {
  ++space->metrics.collection_count;
}

MTKX_END_DECLS

#endif /* MEMTKX_SPACE_SPACE_H */
