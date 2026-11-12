/**
 * @file memtkx/core/vec.h
 * @brief Growable arrays of addresses and slices of text.
 *
 * `mtkx_address_vec` replaces `std::vector<Address>` in the C API: trace
 * callbacks, root providers and SATB queues append to one. Zero
 * initialisation (`mtkx_address_vec v = {0};` or MTKX_ADDRESS_VEC_INIT)
 * produces a valid empty vector.
 */
#ifndef MEMTKX_CORE_VEC_H
#define MEMTKX_CORE_VEC_H

#include <stddef.h>
#include <string.h>

#include "memtkx/core/base.h"
#include "memtkx/core/types.h"

MTKX_BEGIN_DECLS

/** A growable array of addresses. Fields are readable; grow via functions. */
typedef struct mtkx_address_vec {
  mtkx_address *data;
  size_t size;
  size_t capacity;
} mtkx_address_vec;

/** Static initialiser for an empty vector. */
#define MTKX_ADDRESS_VEC_INIT {NULL, 0, 0}

/** Initialises @p vec as empty (no allocation). */
MTKX_INLINE void mtkx_address_vec_init(mtkx_address_vec *vec) {
  vec->data = NULL;
  vec->size = 0;
  vec->capacity = 0;
}

/** Releases storage and leaves @p vec empty. */
MTKX_API void mtkx_address_vec_destroy(mtkx_address_vec *vec);

/** Ensures capacity for at least @p capacity elements. */
MTKX_API mtkx_status mtkx_address_vec_reserve(mtkx_address_vec *vec, size_t capacity);

/** Appends @p value. */
MTKX_API mtkx_status mtkx_address_vec_push(mtkx_address_vec *vec, mtkx_address value);

/** Appends @p count values from @p values. */
MTKX_API mtkx_status mtkx_address_vec_append(mtkx_address_vec *vec,
                                             const mtkx_address *values, size_t count);

/** Removes and returns the last element (the vector must be non-empty). */
MTKX_INLINE mtkx_address mtkx_address_vec_pop(mtkx_address_vec *vec) {
  return vec->data[--vec->size];
}

/** Removes all elements, keeping capacity. */
MTKX_INLINE void mtkx_address_vec_clear(mtkx_address_vec *vec) { vec->size = 0; }

/** @return true when the vector has no elements. */
MTKX_INLINE bool mtkx_address_vec_empty(const mtkx_address_vec *vec) {
  return vec->size == 0;
}

/** Exchanges the contents of two vectors (constant time). */
MTKX_INLINE void mtkx_address_vec_swap(mtkx_address_vec *a, mtkx_address_vec *b) {
  mtkx_address_vec tmp = *a;
  *a = *b;
  *b = tmp;
}

/** A non-owning view of text (C++: `std::string_view`). */
typedef struct mtkx_strview {
  const char *data;
  size_t length;
} mtkx_strview;

/** Builds a view of @p length bytes at @p data. */
MTKX_INLINE mtkx_strview mtkx_strview_make(const char *data, size_t length) {
  mtkx_strview v;
  v.data = data;
  v.length = length;
  return v;
}

/** Builds a view of a NUL-terminated string (NULL gives an empty view). */
MTKX_INLINE mtkx_strview mtkx_strview_cstr(const char *text) {
  return mtkx_strview_make(text, text != NULL ? strlen(text) : 0u);
}

/** @return true when the views hold the same bytes. */
MTKX_INLINE bool mtkx_strview_equal(mtkx_strview a, mtkx_strview b) {
  return a.length == b.length &&
         (a.length == 0 || memcmp(a.data, b.data, a.length) == 0);
}

/** @return true when the view equals a NUL-terminated string. */
MTKX_INLINE bool mtkx_strview_equal_cstr(mtkx_strview a, const char *text) {
  return mtkx_strview_equal(a, mtkx_strview_cstr(text));
}

MTKX_END_DECLS

#endif /* MEMTKX_CORE_VEC_H */
