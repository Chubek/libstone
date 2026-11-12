/*
 * Internal helpers shared by MemTKX translation units. Not installed.
 */
#ifndef MEMTKX_INTERNAL_COMMON_H
#define MEMTKX_INTERNAL_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "memtkx/core/base.h"

/* Overflow-checked size multiplication: returns false on overflow. */
static inline bool mtkx__mul_size(size_t a, size_t b, size_t *out) {
  if (a != 0 && b > SIZE_MAX / a) {
    return false;
  }
  *out = a * b;
  return true;
}

/*
 * Grows a dynamic array so that it can hold at least `needed` elements of
 * `elem_size` bytes. `*data` and `*capacity` are updated on success.
 */
static inline mtkx_status mtkx__grow(void **data, size_t *capacity, size_t needed,
                                     size_t elem_size) {
  size_t new_capacity;
  size_t bytes;
  void *grown;
  if (needed <= *capacity) {
    return MTKX_OK;
  }
  new_capacity = *capacity < 8 ? 8 : *capacity;
  while (new_capacity < needed) {
    if (new_capacity > SIZE_MAX / 2) {
      new_capacity = needed;
      break;
    }
    new_capacity *= 2;
  }
  if (!mtkx__mul_size(new_capacity, elem_size, &bytes)) {
    return MTKX_ERR_NOMEM;
  }
  grown = mtkx_realloc(*data, bytes);
  if (grown == NULL) {
    return MTKX_ERR_NOMEM;
  }
  *data = grown;
  *capacity = new_capacity;
  return MTKX_OK;
}

#define MTKX__GROW(ptr_lvalue, cap_lvalue, needed)                            \
  mtkx__grow((void **)&(ptr_lvalue), &(cap_lvalue), (needed), sizeof(*(ptr_lvalue)))

#define MTKX__UNUSED(x) ((void)(x))

#endif /* MEMTKX_INTERNAL_COMMON_H */
