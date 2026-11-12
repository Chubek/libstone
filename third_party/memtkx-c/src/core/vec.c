#include "memtkx/core/vec.h"

#include <string.h>

#include "internal/common.h"

void mtkx_address_vec_destroy(mtkx_address_vec *vec) {
  mtkx_free(vec->data);
  mtkx_address_vec_init(vec);
}

mtkx_status mtkx_address_vec_reserve(mtkx_address_vec *vec, size_t capacity) {
  return MTKX__GROW(vec->data, vec->capacity, capacity);
}

mtkx_status mtkx_address_vec_push(mtkx_address_vec *vec, mtkx_address value) {
  if (vec->size == vec->capacity) {
    mtkx_status status = MTKX__GROW(vec->data, vec->capacity, vec->size + 1);
    if (status != MTKX_OK) {
      return status;
    }
  }
  vec->data[vec->size++] = value;
  return MTKX_OK;
}

mtkx_status mtkx_address_vec_append(mtkx_address_vec *vec, const mtkx_address *values,
                                    size_t count) {
  mtkx_status status;
  if (count == 0) {
    return MTKX_OK;
  }
  if (count > SIZE_MAX - vec->size) {
    return MTKX_ERR_NOMEM;
  }
  status = MTKX__GROW(vec->data, vec->capacity, vec->size + count);
  if (status != MTKX_OK) {
    return status;
  }
  memmove(vec->data + vec->size, values, count * sizeof(mtkx_address));
  vec->size += count;
  return MTKX_OK;
}
