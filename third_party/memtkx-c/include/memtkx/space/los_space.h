/**
 * @file memtkx/space/los_space.h
 * @brief Threshold-filtered bump allocation for large objects.
 */
#ifndef MEMTKX_SPACE_LOS_SPACE_H
#define MEMTKX_SPACE_LOS_SPACE_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/space/bump_pointer.h"
#include "memtkx/space/space.h"

MTKX_BEGIN_DECLS

/** Default minimum request size for the large-object space (4096). */
#define MTKX_DEFAULT_LARGE_OBJECT_THRESHOLD ((size_t)4 * 1024)

/** A large-object space over borrowed storage (C++ `LargeObjectSpace`). */
typedef struct mtkx_los_space {
  mtkx_space base;
  mtkx_bump_allocator allocator;
  size_t threshold;
} mtkx_los_space;

/**
 * @brief Initialises over `[start, end)`. Pass
 *        MTKX_DEFAULT_LARGE_OBJECT_THRESHOLD for the C++ default.
 */
MTKX_API mtkx_status mtkx_los_space_init(mtkx_los_space *space, const char *name,
                                         mtkx_address start, mtkx_address end,
                                         size_t threshold);

/**
 * @brief Allocates a large object.
 * @return MTKX_ALLOC_LARGE_OBJECT_EXCEEDED when below the threshold or
 *         above the whole region; MTKX_ALLOC_OUT_OF_MEMORY when it no
 *         longer fits.
 */
MTKX_API mtkx_alloc_result mtkx_los_space_allocate(mtkx_los_space *space, size_t bytes,
                                                   size_t alignment);

/**
 * @brief Accounts for a deallocation. Capacity is not returned to the
 *        bump allocator; reuse requires mtkx_los_space_reset().
 */
MTKX_API bool mtkx_los_space_deallocate(mtkx_los_space *space, mtkx_address address,
                                        size_t bytes);

/** Resets the bump cursor; metrics are kept. */
MTKX_INLINE void mtkx_los_space_reset(mtkx_los_space *space) {
  mtkx_bump_allocator_reset(&space->allocator);
}

/** @return the threshold. */
MTKX_INLINE size_t mtkx_los_space_threshold(const mtkx_los_space *space) {
  return space->threshold;
}

/** Releases the space's name. */
MTKX_API void mtkx_los_space_destroy(mtkx_los_space *space);

MTKX_END_DECLS

#endif /* MEMTKX_SPACE_LOS_SPACE_H */
