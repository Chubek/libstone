/**
 * @file memtkx/dsl/result.h
 * @brief Result<T, E> and Maybe<T> for C, generated per type by macros.
 *
 * C has no templates, so success/error and optional types are stamped out
 * with MTKX_DEFINE_RESULT / MTKX_DEFINE_MAYBE. Every generated function is
 * `static inline`, so defining a type in a header costs nothing until used.
 *
 * @code
 *   MTKX_DEFINE_RESULT(int_result, int, const char *);
 *
 *   int_result r = int_result_from_ok(21);
 *   if (int_result_is_ok(r)) printf("%d\n", int_result_unwrap(r));
 *   int fallback = int_result_unwrap_or(int_result_from_err("no"), -1);
 * @endcode
 *
 * Generated for MTKX_DEFINE_RESULT(Name, T, E):
 *
 *   | Function                          | C++ original           |
 *   |-----------------------------------|------------------------|
 *   | Name_from_ok(T) / Name_from_err(E)| from_ok / from_err     |
 *   | Name_is_ok(r) / Name_is_err(r)    | is_ok / is_err         |
 *   | Name_unwrap(r)                    | unwrap (panics on Err) |
 *   | Name_unwrap_or(r, fallback)       | unwrap_or              |
 *   | Name_unwrap_err(r)                | (panics on Ok)         |
 *   | Name_get(r, &out) / Name_get_err  | non-panicking access   |
 *   | Name_map(r, fn, ctx)              | map (T -> T)           |
 *   | Name_map_err(r, fn, ctx)          | map_err (E -> E)       |
 *   | Name_and_then(r, fn, ctx)         | and_then (T -> Name)   |
 *
 * Mapping to a *different* result type uses MTKX_RESULT_MAP /
 * MTKX_RESULT_MAP_ERR / MTKX_RESULT_AND_THEN, which name the output type.
 *
 * Unlike the C++ original, Result<T, T> (same success and error types)
 * works: the active branch is a flag, not a type-indexed variant.
 */
#ifndef MEMTKX_DSL_RESULT_H
#define MEMTKX_DSL_RESULT_H

#include <string.h>

#include "memtkx/core/base.h"

/** @internal Generated helpers may legitimately go unused. */
#define MTKX_GENERATED_ MTKX_INLINE MTKX_UNUSED

/* ------------------------------------------------------------------ */
/*  Result                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Defines `Name`, a success-or-error value holding a T or an E.
 *
 * Fields are public: `ok`, `value` (valid when ok), `error` (valid when
 * !ok). Prefer the generated accessors.
 */
#define MTKX_DEFINE_RESULT(Name, T, E)                                        \
  typedef struct Name {                                                       \
    bool ok;                                                                  \
    T value;                                                                  \
    E error;                                                                  \
  } Name;                                                                     \
  MTKX_GENERATED_ Name Name##_from_ok(T value) {                              \
    Name r;                                                                   \
    memset(&r, 0, sizeof r);                                                  \
    r.ok = true;                                                              \
    r.value = value;                                                          \
    return r;                                                                 \
  }                                                                           \
  MTKX_GENERATED_ Name Name##_from_err(E error) {                             \
    Name r;                                                                   \
    memset(&r, 0, sizeof r);                                                  \
    r.ok = false;                                                             \
    r.error = error;                                                          \
    return r;                                                                 \
  }                                                                           \
  MTKX_GENERATED_ bool Name##_is_ok(Name r) { return r.ok; }                  \
  MTKX_GENERATED_ bool Name##_is_err(Name r) { return !r.ok; }                \
  MTKX_GENERATED_ T Name##_unwrap(Name r) {                                   \
    if (!r.ok) {                                                              \
      mtkx_panic(#Name "_unwrap called on Err");                              \
    }                                                                         \
    return r.value;                                                           \
  }                                                                           \
  MTKX_GENERATED_ T Name##_unwrap_or(Name r, T fallback) {                    \
    return r.ok ? r.value : fallback;                                         \
  }                                                                           \
  MTKX_GENERATED_ E Name##_unwrap_err(Name r) {                               \
    if (r.ok) {                                                               \
      mtkx_panic(#Name "_unwrap_err called on Ok");                           \
    }                                                                         \
    return r.error;                                                           \
  }                                                                           \
  MTKX_GENERATED_ bool Name##_get(Name r, T *out) {                           \
    if (r.ok && out != NULL) {                                                \
      *out = r.value;                                                         \
    }                                                                         \
    return r.ok;                                                              \
  }                                                                           \
  MTKX_GENERATED_ bool Name##_get_err(Name r, E *out) {                       \
    if (!r.ok && out != NULL) {                                               \
      *out = r.error;                                                         \
    }                                                                         \
    return !r.ok;                                                             \
  }                                                                           \
  MTKX_GENERATED_ Name Name##_map(Name r, T (*fn)(T, void *), void *ctx) {    \
    return r.ok ? Name##_from_ok(fn(r.value, ctx)) : r;                       \
  }                                                                           \
  MTKX_GENERATED_ Name Name##_map_err(Name r, E (*fn)(E, void *), void *ctx) { \
    return r.ok ? r : Name##_from_err(fn(r.error, ctx));                      \
  }                                                                           \
  MTKX_GENERATED_ Name Name##_and_then(Name r, Name (*fn)(T, void *),         \
                                   void *ctx) {                               \
    return r.ok ? fn(r.value, ctx) : r;                                       \
  }                                                                           \
  typedef int Name##_mtkx_result_defined_

/**
 * @brief Maps an Ok value into another result type `Out` (same error type).
 *        Evaluates @p r more than once; pass a variable.
 */
#define MTKX_RESULT_MAP(Out, r, fn, ctx)                                      \
  ((r).ok ? Out##_from_ok((fn)((r).value, (ctx))) : Out##_from_err((r).error))

/**
 * @brief Maps an Err value into another result type `Out` (same value type).
 *        Evaluates @p r more than once; pass a variable.
 */
#define MTKX_RESULT_MAP_ERR(Out, r, fn, ctx)                                  \
  ((r).ok ? Out##_from_ok((r).value) : Out##_from_err((fn)((r).error, (ctx))))

/**
 * @brief Chains a function returning result type `Out` (same error type).
 *        Evaluates @p r more than once; pass a variable.
 */
#define MTKX_RESULT_AND_THEN(Out, r, fn, ctx)                                 \
  ((r).ok ? (fn)((r).value, (ctx)) : Out##_from_err((r).error))

/* ------------------------------------------------------------------ */
/*  Maybe                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Defines `Name`, an optional T with monadic helpers.
 *
 * Generated: Name_some, Name_none, Name_has_value, Name_get,
 * Name_unwrap (panics when empty), Name_or_else, Name_filter,
 * Name_map (T -> T) and Name_flat_map (T -> Name).
 */
#define MTKX_DEFINE_MAYBE(Name, T)                                            \
  typedef struct Name {                                                       \
    bool has_value;                                                           \
    T value;                                                                  \
  } Name;                                                                     \
  MTKX_GENERATED_ Name Name##_some(T value) {                                 \
    Name m;                                                                   \
    memset(&m, 0, sizeof m);                                                  \
    m.has_value = true;                                                       \
    m.value = value;                                                          \
    return m;                                                                 \
  }                                                                           \
  MTKX_GENERATED_ Name Name##_none(void) {                                    \
    Name m;                                                                   \
    memset(&m, 0, sizeof m);                                                  \
    return m;                                                                 \
  }                                                                           \
  MTKX_GENERATED_ bool Name##_has_value(Name m) { return m.has_value; }       \
  MTKX_GENERATED_ bool Name##_get(Name m, T *out) {                           \
    if (m.has_value && out != NULL) {                                         \
      *out = m.value;                                                         \
    }                                                                         \
    return m.has_value;                                                       \
  }                                                                           \
  MTKX_GENERATED_ T Name##_unwrap(Name m) {                                   \
    if (!m.has_value) {                                                       \
      mtkx_panic(#Name "_unwrap called on an empty value");                   \
    }                                                                         \
    return m.value;                                                           \
  }                                                                           \
  MTKX_GENERATED_ T Name##_or_else(Name m, T fallback) {                      \
    return m.has_value ? m.value : fallback;                                  \
  }                                                                           \
  MTKX_GENERATED_ Name Name##_filter(Name m, bool (*pred)(T, void *),         \
                                 void *ctx) {                                 \
    return (m.has_value && pred(m.value, ctx)) ? m : Name##_none();           \
  }                                                                           \
  MTKX_GENERATED_ Name Name##_map(Name m, T (*fn)(T, void *), void *ctx) {    \
    return m.has_value ? Name##_some(fn(m.value, ctx)) : m;                   \
  }                                                                           \
  MTKX_GENERATED_ Name Name##_flat_map(Name m, Name (*fn)(T, void *),         \
                                   void *ctx) {                               \
    return m.has_value ? fn(m.value, ctx) : Name##_none();                    \
  }                                                                           \
  typedef int Name##_mtkx_maybe_defined_

/**
 * @brief Maps a Maybe into another Maybe type `Out`.
 *        Evaluates @p m more than once; pass a variable.
 */
#define MTKX_MAYBE_MAP(Out, m, fn, ctx)                                       \
  ((m).has_value ? Out##_some((fn)((m).value, (ctx))) : Out##_none())

/**
 * @brief Chains a function returning Maybe type `Out`.
 *        Evaluates @p m more than once; pass a variable.
 */
#define MTKX_MAYBE_FLAT_MAP(Out, m, fn, ctx)                                  \
  ((m).has_value ? (fn)((m).value, (ctx)) : Out##_none())

MTKX_BEGIN_DECLS

/* Ready-made instances for common scalar types. */
MTKX_DEFINE_MAYBE(mtkx_maybe_int, int);
MTKX_DEFINE_MAYBE(mtkx_maybe_long, long);
MTKX_DEFINE_MAYBE(mtkx_maybe_size, size_t);
MTKX_DEFINE_MAYBE(mtkx_maybe_uintptr, uintptr_t);
MTKX_DEFINE_MAYBE(mtkx_maybe_double, double);
MTKX_DEFINE_MAYBE(mtkx_maybe_char, char);

MTKX_END_DECLS

#endif /* MEMTKX_DSL_RESULT_H */
