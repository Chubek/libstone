/**
 * @file memtkx/dsl/memo.h
 * @brief Memoisation (C++ `memoize<Key, Value>(fn)`) and deferred values
 *        (C++ `Lazy<T>`).
 *
 * @code
 *   static void square(const void *key, void *out, void *ctx) {
 *     ++*(int *)ctx;
 *     *(int *)out = *(const int *)key * *(const int *)key;
 *   }
 *
 *   int calls = 0, result, seven = 7;
 *   mtkx_memo memo;
 *   mtkx_memo_init(&memo, sizeof(int), sizeof(int), square, &calls);
 *   mtkx_memo_call(&memo, &seven, &result);   // computes: 49, calls == 1
 *   mtkx_memo_call(&memo, &seven, &result);   // cached:   49, calls == 1
 *   mtkx_memo_destroy(&memo);
 * @endcode
 *
 * Neither type is synchronised.
 */
#ifndef MEMTKX_DSL_MEMO_H
#define MEMTKX_DSL_MEMO_H

#include <stddef.h>

#include "memtkx/core/base.h"

MTKX_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Memoisation                                                       */
/* ------------------------------------------------------------------ */

/** Computes the value for @p key into @p value_out. */
typedef void (*mtkx_memo_fn)(const void *key, void *value_out, void *ctx);

/**
 * @brief A cache from fixed-size keys to fixed-size values.
 *
 * Keys are hashed and compared byte-wise, so a struct key must have its
 * padding zeroed (e.g. memset before filling it). Fields are private.
 */
typedef struct mtkx_memo {
  size_t key_size;
  size_t value_size;
  mtkx_memo_fn fn;
  void *ctx;
  unsigned char *slots; /* private */
  size_t capacity;      /* private */
  size_t count;         /* private */
} mtkx_memo;

/**
 * @brief Initialises an empty cache (no allocation yet).
 * @return MTKX_ERR_INVALID_ARGUMENT for a zero key size or NULL fn.
 */
MTKX_API mtkx_status mtkx_memo_init(mtkx_memo *memo, size_t key_size, size_t value_size,
                                    mtkx_memo_fn fn, void *ctx);

/**
 * @brief Copies the cached value for @p key into @p value_out, computing
 *        and caching it first if needed.
 * @return MTKX_OK; MTKX_ERR_NOMEM when the value was computed into
 *         @p value_out but could not be cached.
 */
MTKX_API mtkx_status mtkx_memo_call(mtkx_memo *memo, const void *key, void *value_out);

/** @return true when @p key is cached. */
MTKX_API bool mtkx_memo_contains(const mtkx_memo *memo, const void *key);

/** @return the number of cached entries. */
MTKX_API size_t mtkx_memo_size(const mtkx_memo *memo);

/** Removes every cached entry (C++ `clear_cache()`). */
MTKX_API void mtkx_memo_clear_cache(mtkx_memo *memo);

/** Releases the cache's storage. */
MTKX_API void mtkx_memo_destroy(mtkx_memo *memo);

/* ------------------------------------------------------------------ */
/*  Lazy                                                              */
/* ------------------------------------------------------------------ */

/** Produces the deferred value into @p value_out. */
typedef void (*mtkx_lazy_fn)(void *value_out, void *ctx);

/** A single deferred computation writing into caller storage. */
typedef struct mtkx_lazy {
  mtkx_lazy_fn producer;
  void *ctx;
  void *storage;
  bool computed;
} mtkx_lazy;

/** Initialises @p lazy; the value will be written to @p storage on first get. */
MTKX_INLINE void mtkx_lazy_init(mtkx_lazy *lazy, void *storage, mtkx_lazy_fn producer,
                                void *ctx) {
  lazy->producer = producer;
  lazy->ctx = ctx;
  lazy->storage = storage;
  lazy->computed = false;
}

/** Computes the value on first access and returns a pointer to it. */
MTKX_INLINE void *mtkx_lazy_get(mtkx_lazy *lazy) {
  if (!lazy->computed) {
    lazy->producer(lazy->storage, lazy->ctx);
    lazy->computed = true;
  }
  return lazy->storage;
}

/** Alias of mtkx_lazy_get(). */
MTKX_INLINE void *mtkx_lazy_force(mtkx_lazy *lazy) { return mtkx_lazy_get(lazy); }

/** @return whether the value has been computed. */
MTKX_INLINE bool mtkx_lazy_is_computed(const mtkx_lazy *lazy) { return lazy->computed; }

MTKX_END_DECLS

#endif /* MEMTKX_DSL_MEMO_H */
