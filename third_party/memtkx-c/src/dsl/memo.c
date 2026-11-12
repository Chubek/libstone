#include "memtkx/dsl/memo.h"

#include <stdlib.h>
#include <string.h>

#include "internal/common.h"

/* Slot layout: [1 byte used][key_size bytes][value_size bytes] */

static size_t mtkx__memo_slot_size(const mtkx_memo *memo) {
  return 1 + memo->key_size + memo->value_size;
}

static size_t mtkx__fnv1a(const void *data, size_t size) {
  const unsigned char *p = (const unsigned char *)data;
  uint64_t h = 1469598103934665603ULL;
  size_t i;
  for (i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return (size_t)(h ^ (h >> 32));
}

mtkx_status mtkx_memo_init(mtkx_memo *memo, size_t key_size, size_t value_size,
                           mtkx_memo_fn fn, void *ctx) {
  memset(memo, 0, sizeof *memo);
  if (key_size == 0 || fn == NULL) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  memo->key_size = key_size;
  memo->value_size = value_size;
  memo->fn = fn;
  memo->ctx = ctx;
  return MTKX_OK;
}

static unsigned char *mtkx__memo_find(const mtkx_memo *memo, const void *key,
                                      bool *found) {
  size_t slot_size = mtkx__memo_slot_size(memo);
  size_t mask = memo->capacity - 1;
  size_t i = mtkx__fnv1a(key, memo->key_size) & mask;
  for (;;) {
    unsigned char *slot = memo->slots + i * slot_size;
    if (!slot[0]) {
      *found = false;
      return slot;
    }
    if (memcmp(slot + 1, key, memo->key_size) == 0) {
      *found = true;
      return slot;
    }
    i = (i + 1) & mask;
  }
}

static mtkx_status mtkx__memo_grow(mtkx_memo *memo) {
  size_t new_capacity = memo->capacity == 0 ? 16 : memo->capacity * 2;
  size_t slot_size = mtkx__memo_slot_size(memo);
  unsigned char *old = memo->slots;
  size_t old_capacity = memo->capacity;
  unsigned char *slots;
  size_t i;
  if (memo->capacity > SIZE_MAX / 2 / slot_size) {
    return MTKX_ERR_NOMEM;
  }
  slots = (unsigned char *)mtkx_calloc(new_capacity, slot_size);
  if (slots == NULL) {
    return MTKX_ERR_NOMEM;
  }
  memo->slots = slots;
  memo->capacity = new_capacity;
  for (i = 0; i < old_capacity; ++i) {
    unsigned char *src = old + i * slot_size;
    if (src[0]) {
      bool found;
      unsigned char *dst = mtkx__memo_find(memo, src + 1, &found);
      memcpy(dst, src, slot_size);
    }
  }
  mtkx_free(old);
  return MTKX_OK;
}

mtkx_status mtkx_memo_call(mtkx_memo *memo, const void *key, void *value_out) {
  bool found = false;
  unsigned char *slot;
  if (memo->count != 0) {
    slot = mtkx__memo_find(memo, key, &found);
    if (found) {
      if (memo->value_size != 0) {
        memcpy(value_out, slot + 1 + memo->key_size, memo->value_size);
      }
      return MTKX_OK;
    }
  }
  memo->fn(key, value_out, memo->ctx);
  if ((memo->count + 1) * 4 > memo->capacity * 3) {
    if (mtkx__memo_grow(memo) != MTKX_OK) {
      return MTKX_ERR_NOMEM;
    }
  }
  slot = mtkx__memo_find(memo, key, &found);
  slot[0] = 1;
  memcpy(slot + 1, key, memo->key_size);
  if (memo->value_size != 0) {
    memcpy(slot + 1 + memo->key_size, value_out, memo->value_size);
  }
  ++memo->count;
  return MTKX_OK;
}

bool mtkx_memo_contains(const mtkx_memo *memo, const void *key) {
  bool found = false;
  if (memo->count == 0) {
    return false;
  }
  (void)mtkx__memo_find(memo, key, &found);
  return found;
}

size_t mtkx_memo_size(const mtkx_memo *memo) { return memo->count; }

void mtkx_memo_clear_cache(mtkx_memo *memo) {
  if (memo->slots != NULL) {
    memset(memo->slots, 0, memo->capacity * mtkx__memo_slot_size(memo));
  }
  memo->count = 0;
}

void mtkx_memo_destroy(mtkx_memo *memo) {
  mtkx_free(memo->slots);
  memo->slots = NULL;
  memo->capacity = 0;
  memo->count = 0;
}
