#include "memtkx/space/free_list.h"

#include <string.h>

#include "internal/common.h"

/* ------------------------------------------------------------------ */
/*  FreeListAllocator                                                 */
/* ------------------------------------------------------------------ */

mtkx_status mtkx_free_list_allocator_init(mtkx_free_list_allocator *allocator,
                                          mtkx_address start, mtkx_address end) {
  memset(allocator, 0, sizeof *allocator);
  return mtkx_free_list_allocator_reset(allocator, start, end);
}

mtkx_status mtkx_free_list_allocator_reset(mtkx_free_list_allocator *allocator,
                                           mtkx_address start, mtkx_address end) {
  allocator->start = start;
  allocator->limit = end;
  allocator->cell_count = 0;
  allocator->allocations = 0;
  allocator->allocated_bytes = 0;
  if (end > start) {
    if (MTKX__GROW(allocator->cells, allocator->cell_capacity, 1) != MTKX_OK) {
      return MTKX_ERR_NOMEM;
    }
    allocator->cells[0].address = start;
    allocator->cells[0].size = (size_t)(end - start);
    allocator->cell_count = 1;
  }
  return MTKX_OK;
}

void mtkx_free_list_allocator_destroy(mtkx_free_list_allocator *allocator) {
  mtkx_free(allocator->cells);
  memset(allocator, 0, sizeof *allocator);
}

/* Merges address-adjacent neighbours; cells must be sorted by address. */
static void mtkx__coalesce(mtkx_free_list_allocator *a) {
  size_t read;
  size_t write = 0;
  if (a->cell_count < 2) {
    return;
  }
  for (read = 1; read < a->cell_count; ++read) {
    mtkx_free_cell *current = &a->cells[write];
    const mtkx_free_cell next = a->cells[read];
    if (mtkx_free_cell_end(*current) == next.address) {
      current->size += next.size;
    } else {
      a->cells[++write] = next;
    }
  }
  a->cell_count = write + 1;
}

/* Index of the first cell whose address is >= addr (binary search). */
static size_t mtkx__lower_bound(const mtkx_free_list_allocator *a, mtkx_address addr) {
  size_t lo = 0;
  size_t hi = a->cell_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (a->cells[mid].address < addr) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

mtkx_alloc_result mtkx_free_list_allocator_allocate(mtkx_free_list_allocator *allocator,
                                                    size_t bytes, size_t alignment) {
  size_t i;
  if (bytes == 0) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
  }
  if (!mtkx_is_power_of_two(alignment)) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_MISALIGNED);
  }

  for (i = 0; i < allocator->cell_count; ++i) {
    const mtkx_free_cell cell = allocator->cells[i];
    const mtkx_address object = mtkx_align_up(cell.address, alignment);
    size_t padding;
    size_t remainder;
    size_t replacement = 0;
    mtkx_free_cell parts[2];

    if (object < cell.address || (size_t)(object - cell.address) > cell.size) {
      continue;
    }
    padding = (size_t)(object - cell.address);
    if (bytes > cell.size - padding) {
      continue;
    }

    /* Replace the cell with its prefix padding and suffix remainder. Both
     * lie inside the original cell, so address order is preserved and no
     * neighbour can become adjacent: this is equivalent to the C++
     * erase / push_back / sort / coalesce sequence. */
    if (padding != 0) {
      parts[replacement].address = cell.address;
      parts[replacement].size = padding;
      ++replacement;
    }
    remainder = cell.size - padding - bytes;
    if (remainder != 0) {
      parts[replacement].address = object + bytes;
      parts[replacement].size = remainder;
      ++replacement;
    }
    if (replacement == 2) {
      if (MTKX__GROW(allocator->cells, allocator->cell_capacity, allocator->cell_count + 1) !=
          MTKX_OK) {
        return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
      }
      memmove(&allocator->cells[i + 2], &allocator->cells[i + 1],
              (allocator->cell_count - i - 1) * sizeof(mtkx_free_cell));
      allocator->cells[i] = parts[0];
      allocator->cells[i + 1] = parts[1];
      ++allocator->cell_count;
    } else if (replacement == 1) {
      allocator->cells[i] = parts[0];
    } else {
      memmove(&allocator->cells[i], &allocator->cells[i + 1],
              (allocator->cell_count - i - 1) * sizeof(mtkx_free_cell));
      --allocator->cell_count;
    }
    mtkx__coalesce(allocator);

    ++allocator->allocations;
    allocator->allocated_bytes += bytes;
    return mtkx_alloc_result_from_ok(object);
  }
  return mtkx_alloc_result_from_err(MTKX_ALLOC_OUT_OF_MEMORY);
}

bool mtkx_free_list_allocator_free(mtkx_free_list_allocator *allocator, mtkx_address address,
                                   size_t bytes) {
  size_t at;
  if (address < allocator->start || bytes == 0 || address >= allocator->limit ||
      bytes > (size_t)(allocator->limit - address)) {
    return false;
  }
  if (MTKX__GROW(allocator->cells, allocator->cell_capacity, allocator->cell_count + 1) !=
      MTKX_OK) {
    return false;
  }
  at = mtkx__lower_bound(allocator, address);
  memmove(&allocator->cells[at + 1], &allocator->cells[at],
          (allocator->cell_count - at) * sizeof(mtkx_free_cell));
  allocator->cells[at].address = address;
  allocator->cells[at].size = bytes;
  ++allocator->cell_count;
  mtkx__coalesce(allocator);
  return true;
}

size_t mtkx_free_list_allocator_free_bytes(const mtkx_free_list_allocator *a) {
  size_t total = 0;
  size_t i;
  for (i = 0; i < a->cell_count; ++i) {
    total += a->cells[i].size;
  }
  return total;
}

/* ------------------------------------------------------------------ */
/*  FreeListSpace                                                     */
/* ------------------------------------------------------------------ */

static mtkx_alloc_result mtkx__fl_space_allocate_impl(mtkx_space *base, size_t bytes,
                                                      size_t alignment) {
  mtkx_free_list_space *space = (mtkx_free_list_space *)base;
  mtkx_alloc_result result =
      mtkx_free_list_allocator_allocate(&space->allocator, bytes, alignment);
  if (result.ok) {
    mtkx_space_record_allocation(base, bytes);
  }
  return result;
}

static void mtkx__fl_space_destroy(mtkx_space *base) {
  mtkx_free_list_allocator_destroy(&((mtkx_free_list_space *)base)->allocator);
}

static const mtkx_space_ops mtkx__fl_space_ops = {mtkx__fl_space_allocate_impl,
                                                  mtkx__fl_space_destroy};

mtkx_status mtkx_free_list_space_init(mtkx_free_list_space *space, const char *name,
                                      mtkx_address start, mtkx_address end) {
  mtkx_status status = mtkx_free_list_allocator_init(&space->allocator, start, end);
  if (status != MTKX_OK) {
    return status;
  }
  status = mtkx_space_init(&space->base, &mtkx__fl_space_ops, name, MTKX_SPACE_FREE_LIST,
                           mtkx_range(start, end), MTKX_MAX_ALIGN);
  if (status != MTKX_OK) {
    mtkx_free_list_allocator_destroy(&space->allocator);
  }
  return status;
}

mtkx_alloc_result mtkx_free_list_space_allocate(mtkx_free_list_space *space, size_t bytes,
                                                size_t alignment) {
  return mtkx_space_allocate(&space->base, bytes, alignment);
}

bool mtkx_free_list_space_deallocate(mtkx_free_list_space *space, mtkx_address address,
                                     size_t bytes) {
  if (!mtkx_free_list_allocator_free(&space->allocator, address, bytes)) {
    return false;
  }
  mtkx_space_record_deallocation(&space->base, bytes);
  return true;
}

mtkx_status mtkx_free_list_space_reset(mtkx_free_list_space *space) {
  mtkx_address_range region = mtkx_space_region(&space->base);
  return mtkx_free_list_allocator_reset(&space->allocator, region.start, region.end);
}

void mtkx_free_list_space_destroy(mtkx_free_list_space *space) {
  mtkx_space_destroy(&space->base);
}

/* ------------------------------------------------------------------ */
/*  SegregatedFreeListAllocator                                       */
/* ------------------------------------------------------------------ */

const size_t mtkx_segregated_size_classes[MTKX_SEGREGATED_CLASS_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 4096};

mtkx_status mtkx_segregated_allocator_init(mtkx_segregated_allocator *allocator,
                                           mtkx_address start, mtkx_address end) {
  memset(allocator, 0, sizeof *allocator);
  return mtkx_segregated_allocator_reset(allocator, start, end);
}

mtkx_status mtkx_segregated_allocator_reset(mtkx_segregated_allocator *allocator,
                                            mtkx_address start, mtkx_address end) {
  size_t i;
  size_t total;
  size_t per_class;
  mtkx_address cursor;

  allocator->start = start;
  allocator->limit = end;
  for (i = 0; i < MTKX_SEGREGATED_CLASS_COUNT; ++i) {
    /* keep the cell arrays for reuse; reset to an empty region */
    (void)mtkx_free_list_allocator_reset(&allocator->bins[i], 0, 0);
    allocator->ranges[i] = mtkx_range(0, 0);
  }
  if (end <= start) {
    return MTKX_OK;
  }

  total = (size_t)(end - start);
  per_class = total / MTKX_SEGREGATED_CLASS_COUNT;
  cursor = start;
  for (i = 0; i < MTKX_SEGREGATED_CLASS_COUNT; ++i) {
    size_t class_bytes =
        i + 1 == MTKX_SEGREGATED_CLASS_COUNT ? (size_t)(end - cursor) : per_class;
    mtkx_address class_end = cursor + class_bytes;
    allocator->ranges[i] = mtkx_range(cursor, class_end);
    if (mtkx_free_list_allocator_reset(&allocator->bins[i], cursor, class_end) != MTKX_OK) {
      return MTKX_ERR_NOMEM;
    }
    cursor = class_end;
  }
  return MTKX_OK;
}

void mtkx_segregated_allocator_destroy(mtkx_segregated_allocator *allocator) {
  size_t i;
  for (i = 0; i < MTKX_SEGREGATED_CLASS_COUNT; ++i) {
    mtkx_free_list_allocator_destroy(&allocator->bins[i]);
  }
  memset(allocator, 0, sizeof *allocator);
}

mtkx_alloc_result mtkx_segregated_allocator_allocate(mtkx_segregated_allocator *allocator,
                                                     size_t bytes, size_t alignment) {
  size_t i;
  if (bytes == 0) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
  }
  if (!mtkx_is_power_of_two(alignment)) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_MISALIGNED);
  }
  for (i = 0; i < MTKX_SEGREGATED_CLASS_COUNT; ++i) {
    mtkx_alloc_result result;
    if (mtkx_segregated_size_classes[i] < bytes) {
      continue;
    }
    result = mtkx_free_list_allocator_allocate(&allocator->bins[i], bytes, alignment);
    if (result.ok) {
      return result;
    }
  }
  return mtkx_alloc_result_from_err(MTKX_ALLOC_OUT_OF_MEMORY);
}

bool mtkx_segregated_allocator_free(mtkx_segregated_allocator *allocator,
                                    mtkx_address address, size_t bytes) {
  size_t i;
  for (i = 0; i < MTKX_SEGREGATED_CLASS_COUNT; ++i) {
    if (mtkx_range_contains(allocator->ranges[i], address)) {
      return mtkx_free_list_allocator_free(&allocator->bins[i], address, bytes);
    }
  }
  return false;
}
