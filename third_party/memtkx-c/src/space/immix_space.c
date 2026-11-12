#include "memtkx/space/immix_space.h"

#include <string.h>

#include "internal/common.h"

/* ------------------------------------------------------------------ */
/*  Blocks                                                            */
/* ------------------------------------------------------------------ */

void mtkx_immix_block_init(mtkx_immix_block *block, mtkx_address start, mtkx_address end) {
  block->start = start;
  block->limit = end;
  block->cursor = start;
  memset(block->line_marks, 0, sizeof block->line_marks);
}

static size_t mtkx__line_index(const mtkx_immix_block *block, mtkx_address address) {
  return (size_t)(address - block->start) / MTKX_IMMIX_LINE_BYTES;
}

static void mtkx__mark_lines(mtkx_immix_block *block, mtkx_address address, size_t bytes) {
  size_t first;
  size_t last;
  size_t i;
  if (bytes == 0) {
    return; /* a zero-byte extent covers no line */
  }
  first = mtkx__line_index(block, address);
  last = mtkx__line_index(block, address + bytes - 1);
  for (i = first; i <= last && i < MTKX_IMMIX_LINES_PER_BLOCK; ++i) {
    block->line_marks[i] = true;
  }
}

bool mtkx_immix_block_allocate(mtkx_immix_block *block, size_t bytes, size_t alignment,
                               mtkx_address *out) {
  mtkx_address aligned = mtkx_align_up(block->cursor, alignment);
  if (aligned < block->cursor || !mtkx_can_allocate(aligned, block->limit, bytes, 1)) {
    return false;
  }
  mtkx__mark_lines(block, aligned, bytes);
  block->cursor = aligned + bytes;
  *out = aligned;
  return true;
}

void mtkx_immix_block_reset(mtkx_immix_block *block) {
  block->cursor = block->start;
  memset(block->line_marks, 0, sizeof block->line_marks);
}

void mtkx_immix_block_clear_marks(mtkx_immix_block *block) {
  memset(block->line_marks, 0, sizeof block->line_marks);
}

void mtkx_immix_block_mark(mtkx_immix_block *block, mtkx_address address, size_t bytes) {
  if (address < block->start || address + bytes > block->limit) {
    return;
  }
  mtkx__mark_lines(block, address, bytes);
}

size_t mtkx_immix_block_used_bytes(const mtkx_immix_block *block) {
  return block->cursor >= block->start ? (size_t)(block->cursor - block->start) : 0u;
}

size_t mtkx_immix_block_remaining_bytes(const mtkx_immix_block *block) {
  return block->cursor >= block->limit ? 0u : (size_t)(block->limit - block->cursor);
}

size_t mtkx_immix_block_marked_lines(const mtkx_immix_block *block) {
  size_t i;
  size_t n = 0;
  for (i = 0; i < MTKX_IMMIX_LINES_PER_BLOCK; ++i) {
    n += block->line_marks[i] ? 1u : 0u;
  }
  return n;
}

size_t mtkx_immix_block_marked_bytes(const mtkx_immix_block *block) {
  return mtkx_immix_block_marked_lines(block) * MTKX_IMMIX_LINE_BYTES;
}

/* ------------------------------------------------------------------ */
/*  Space                                                             */
/* ------------------------------------------------------------------ */

static mtkx_alloc_result mtkx__immix_allocate_impl(mtkx_space *base, size_t bytes,
                                                   size_t alignment) {
  mtkx_immix_space *space = (mtkx_immix_space *)base;
  size_t i;
  if (bytes > MTKX_IMMIX_BLOCK_BYTES) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_LARGE_OBJECT_EXCEEDED);
  }
  for (i = 0; i < space->block_count; ++i) {
    mtkx_address address;
    if (mtkx_immix_block_allocate(&space->blocks[i], bytes, alignment, &address)) {
      mtkx_space_record_allocation(base, bytes);
      return mtkx_alloc_result_from_ok(address);
    }
  }
  return mtkx_alloc_result_from_err(MTKX_ALLOC_OUT_OF_MEMORY);
}

static void mtkx__immix_destroy(mtkx_space *base) {
  mtkx_immix_space *space = (mtkx_immix_space *)base;
  mtkx_free(space->blocks);
  mtkx_free(space->storage);
  space->blocks = NULL;
  space->storage = NULL;
  space->block_count = 0;
  space->storage_bytes = 0;
}

static const mtkx_space_ops mtkx__immix_ops = {mtkx__immix_allocate_impl,
                                               mtkx__immix_destroy};

static size_t mtkx__aligned_block_bytes(size_t bytes) {
  if (bytes <= MTKX_IMMIX_BLOCK_BYTES) {
    return MTKX_IMMIX_BLOCK_BYTES;
  }
  return ((bytes + MTKX_IMMIX_BLOCK_BYTES - 1) / MTKX_IMMIX_BLOCK_BYTES) *
         MTKX_IMMIX_BLOCK_BYTES;
}

mtkx_status mtkx_immix_space_init(mtkx_immix_space *space, size_t bytes) {
  mtkx_address raw;
  mtkx_address base;
  size_t usable;
  size_t i;
  mtkx_status status;

  memset(space, 0, sizeof *space);
  status = mtkx_space_init(&space->base, &mtkx__immix_ops, "immix", MTKX_SPACE_IMMIX,
                           mtkx_range(0, 0), MTKX_IMMIX_LINE_BYTES);
  if (status != MTKX_OK) {
    return status;
  }
  if (bytes > SIZE_MAX - 2 * MTKX_IMMIX_BLOCK_BYTES) {
    mtkx_space_destroy(&space->base);
    return MTKX_ERR_NOMEM;
  }
  /* same sizing as the C++ ImmixSpace: whole blocks plus line padding */
  space->storage_bytes =
      mtkx__aligned_block_bytes(bytes + MTKX_IMMIX_LINE_BYTES - 1) + MTKX_IMMIX_LINE_BYTES;
  space->storage = mtkx_calloc(space->storage_bytes, 1);
  if (space->storage == NULL) {
    mtkx_space_destroy(&space->base);
    return MTKX_ERR_NOMEM;
  }
  raw = mtkx_as_address(space->storage);
  base = mtkx_align_up(raw, MTKX_IMMIX_LINE_BYTES);
  usable = space->storage_bytes - (size_t)(base - raw);
  space->block_count = usable / MTKX_IMMIX_BLOCK_BYTES;
  space->base.descriptor.region =
      mtkx_range(base, base + space->block_count * MTKX_IMMIX_BLOCK_BYTES);

  if (space->block_count != 0) {
    space->blocks =
        (mtkx_immix_block *)mtkx_malloc(space->block_count * sizeof(mtkx_immix_block));
    if (space->blocks == NULL) {
      mtkx_space_destroy(&space->base);
      return MTKX_ERR_NOMEM;
    }
  }
  for (i = 0; i < space->block_count; ++i) {
    mtkx_address block_start = base + i * MTKX_IMMIX_BLOCK_BYTES;
    mtkx_immix_block_init(&space->blocks[i], block_start,
                          block_start + MTKX_IMMIX_BLOCK_BYTES);
  }
  return MTKX_OK;
}

void mtkx_immix_space_destroy(mtkx_immix_space *space) { mtkx_space_destroy(&space->base); }

mtkx_alloc_result mtkx_immix_space_allocate(mtkx_immix_space *space, size_t bytes,
                                            size_t alignment) {
  return mtkx_space_allocate(&space->base, bytes, alignment);
}

bool mtkx_immix_space_mark(mtkx_immix_space *space, mtkx_address address, size_t bytes) {
  size_t i;
  for (i = 0; i < space->block_count; ++i) {
    mtkx_immix_block *block = &space->blocks[i];
    if (address >= block->start && address < block->limit) {
      mtkx_immix_block_mark(block, address, bytes);
      return true;
    }
  }
  return false;
}

void mtkx_immix_space_reset_blocks(mtkx_immix_space *space) {
  size_t i;
  for (i = 0; i < space->block_count; ++i) {
    mtkx_immix_block_reset(&space->blocks[i]);
  }
}

void mtkx_immix_space_clear_marks(mtkx_immix_space *space) {
  size_t i;
  for (i = 0; i < space->block_count; ++i) {
    mtkx_immix_block_clear_marks(&space->blocks[i]);
  }
}

size_t mtkx_immix_space_used_bytes(const mtkx_immix_space *space) {
  size_t i;
  size_t total = 0;
  for (i = 0; i < space->block_count; ++i) {
    total += mtkx_immix_block_used_bytes(&space->blocks[i]);
  }
  return total;
}

size_t mtkx_immix_space_marked_bytes(const mtkx_immix_space *space) {
  size_t i;
  size_t total = 0;
  for (i = 0; i < space->block_count; ++i) {
    total += mtkx_immix_block_marked_bytes(&space->blocks[i]);
  }
  return total;
}
