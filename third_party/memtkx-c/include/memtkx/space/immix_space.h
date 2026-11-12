/**
 * @file memtkx/space/immix_space.h
 * @brief Immix blocks with line marks, and the owning ImmixSpace.
 */
#ifndef MEMTKX_SPACE_IMMIX_SPACE_H
#define MEMTKX_SPACE_IMMIX_SPACE_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/space/space.h"

MTKX_BEGIN_DECLS

/** Bytes per Immix block (32 KiB). */
#define MTKX_IMMIX_BLOCK_BYTES ((size_t)32 * 1024)
/** Bytes per Immix line (256). */
#define MTKX_IMMIX_LINE_BYTES ((size_t)256)
/** Lines per block (128). */
#define MTKX_IMMIX_LINES_PER_BLOCK (MTKX_IMMIX_BLOCK_BYTES / MTKX_IMMIX_LINE_BYTES)

/** One block: a forward cursor and a mark per line (C++ `ImmixBlock`). */
typedef struct mtkx_immix_block {
  mtkx_address start;
  mtkx_address limit;
  mtkx_address cursor;
  bool line_marks[MTKX_IMMIX_LINES_PER_BLOCK];
} mtkx_immix_block;

/** Initialises a block over `[start, end)` with no marks. */
MTKX_API void mtkx_immix_block_init(mtkx_immix_block *block, mtkx_address start,
                                    mtkx_address end);

/**
 * @brief Bumps the cursor and marks the touched lines.
 * @return true and the address in *out on success.
 */
MTKX_API bool mtkx_immix_block_allocate(mtkx_immix_block *block, size_t bytes,
                                        size_t alignment, mtkx_address *out);

/** Resets the cursor and clears marks. */
MTKX_API void mtkx_immix_block_reset(mtkx_immix_block *block);
/** Clears marks only. */
MTKX_API void mtkx_immix_block_clear_marks(mtkx_immix_block *block);
/** Marks the lines covered by `[address, address + bytes)` (ignored if outside). */
MTKX_API void mtkx_immix_block_mark(mtkx_immix_block *block, mtkx_address address,
                                    size_t bytes);
/** @return cursor distance from start. */
MTKX_API size_t mtkx_immix_block_used_bytes(const mtkx_immix_block *block);
/** @return bytes after the cursor. */
MTKX_API size_t mtkx_immix_block_remaining_bytes(const mtkx_immix_block *block);
/** @return the number of marked lines. */
MTKX_API size_t mtkx_immix_block_marked_lines(const mtkx_immix_block *block);
/** @return marked lines * MTKX_IMMIX_LINE_BYTES. */
MTKX_API size_t mtkx_immix_block_marked_bytes(const mtkx_immix_block *block);

/**
 * @brief An Immix space owning its backing storage (C++ `ImmixSpace`).
 *
 * Storage is rounded to whole blocks and aligned to a line boundary, so
 * the block count can exceed the minimum implied by the requested size.
 */
typedef struct mtkx_immix_space {
  mtkx_space base;
  void *storage;       /* private: owned buffer */
  size_t storage_bytes;
  mtkx_immix_block *blocks;
  size_t block_count;
} mtkx_immix_space;

/** Allocates storage for at least @p bytes and builds the blocks. */
MTKX_API mtkx_status mtkx_immix_space_init(mtkx_immix_space *space, size_t bytes);

/** Releases the storage. */
MTKX_API void mtkx_immix_space_destroy(mtkx_immix_space *space);

/**
 * @brief First-fit over blocks.
 * @return MTKX_ALLOC_LARGE_OBJECT_EXCEEDED for requests above one block.
 */
MTKX_API mtkx_alloc_result mtkx_immix_space_allocate(mtkx_immix_space *space, size_t bytes,
                                                     size_t alignment);

/** Marks the lines of a known object; false when no block contains it. */
MTKX_API bool mtkx_immix_space_mark(mtkx_immix_space *space, mtkx_address address,
                                    size_t bytes);
/** Resets every block (releases all allocations). */
MTKX_API void mtkx_immix_space_reset_blocks(mtkx_immix_space *space);
/** Clears every line mark. */
MTKX_API void mtkx_immix_space_clear_marks(mtkx_immix_space *space);
/** @return the number of blocks. */
MTKX_INLINE size_t mtkx_immix_space_block_count(const mtkx_immix_space *space) {
  return space->block_count;
}
/** @return block @p index (NULL when out of range). */
MTKX_INLINE const mtkx_immix_block *mtkx_immix_space_block(const mtkx_immix_space *space,
                                                           size_t index) {
  return index < space->block_count ? &space->blocks[index] : NULL;
}
/** @return the owned storage size including padding. */
MTKX_INLINE size_t mtkx_immix_space_total_bytes(const mtkx_immix_space *space) {
  return space->storage_bytes;
}
/** @return the sum of block cursor consumption. */
MTKX_API size_t mtkx_immix_space_used_bytes(const mtkx_immix_space *space);
/** @return the sum of marked line capacity. */
MTKX_API size_t mtkx_immix_space_marked_bytes(const mtkx_immix_space *space);

MTKX_END_DECLS

#endif /* MEMTKX_SPACE_IMMIX_SPACE_H */
