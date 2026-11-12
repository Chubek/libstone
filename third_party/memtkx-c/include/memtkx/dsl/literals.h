/**
 * @file memtkx/dsl/literals.h
 * @brief Runtime literal-suffix tables (C++ `literal_set(lit<"_km">(f), ...)`).
 *
 * @code
 *   static long double km(long double v, void *ctx) { return v * 1000.0L; }
 *   static long double m(long double v, void *ctx)  { return v; }
 *
 *   const mtkx_literal_entry units[] = { mtkx_lit("_km", km, NULL),
 *                                        mtkx_lit("_m",  m,  NULL) };
 *   mtkx_literal_set set = mtkx_literal_set_make(units, 2);
 *   long double d;
 *   mtkx_literal_set_parse(&set, "3.5_km", &d);   // d == 3500.0L
 * @endcode
 */
#ifndef MEMTKX_DSL_LITERALS_H
#define MEMTKX_DSL_LITERALS_H

#include <stddef.h>

#include "memtkx/core/base.h"

MTKX_BEGIN_DECLS

/** Converts the numeric part of a literal. */
typedef long double (*mtkx_literal_fn)(long double value, void *ctx);

/** One suffix and its conversion (C++ `LiteralEntry`). */
typedef struct mtkx_literal_entry {
  const char *suffix;
  mtkx_literal_fn handler;
  void *ctx;
} mtkx_literal_entry;

/** Builds an entry (C++ `lit<"suffix">(f)`). */
MTKX_INLINE mtkx_literal_entry mtkx_lit(const char *suffix, mtkx_literal_fn handler,
                                        void *ctx) {
  mtkx_literal_entry e;
  e.suffix = suffix;
  e.handler = handler;
  e.ctx = ctx;
  return e;
}

/** An ordered table of caller-owned entries (C++ `LiteralSet`). */
typedef struct mtkx_literal_set {
  const mtkx_literal_entry *entries;
  size_t count;
} mtkx_literal_set;

/** Builds a set view of @p count entries. */
MTKX_INLINE mtkx_literal_set mtkx_literal_set_make(const mtkx_literal_entry *entries,
                                                   size_t count) {
  mtkx_literal_set s;
  s.entries = entries;
  s.count = count;
  return s;
}

/**
 * @brief Parses @p input such as "3.5_km".
 *
 * Entries are tried in order; the first suffix that ends @p input (and is
 * strictly shorter than it) wins. The prefix is converted like strtold /
 * std::stold: leading whitespace is skipped and characters after the
 * number are ignored.
 *
 * @return MTKX_OK; MTKX_ERR_NOT_FOUND when no suffix matched (the C++
 *         version threw); MTKX_ERR_PARSE when the prefix has no number;
 *         MTKX_ERR_RANGE on overflow/underflow; MTKX_ERR_NOMEM.
 */
MTKX_API mtkx_status mtkx_literal_set_parse(const mtkx_literal_set *set,
                                            const char *input, long double *out);

/** Length-delimited variant of mtkx_literal_set_parse(). */
MTKX_API mtkx_status mtkx_literal_set_parse_n(const mtkx_literal_set *set,
                                              const char *input, size_t length,
                                              long double *out);

MTKX_END_DECLS

#endif /* MEMTKX_DSL_LITERALS_H */
