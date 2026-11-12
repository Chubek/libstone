/**
 * @file memtkx/dsl/match.h
 * @brief Dispatch tables (C++ `match(when<K>(h), ..., otherwise(h))`) and
 *        the anchored regex-subset matcher (C++ `pattern<"...">::matches`).
 *
 * @code
 *   enum state { UNMARKED, MARKED, PINNED };
 *   static void to_marked(mtkx_match_key k, void *args, void *out, void *ctx) {
 *     *(enum state *)out = MARKED;
 *   }
 *   static void keep(mtkx_match_key k, void *args, void *out, void *ctx) {
 *     *(enum state *)out = (enum state)k.value;
 *   }
 *
 *   const mtkx_match_clause clauses[] = {
 *     mtkx_when(UNMARKED, to_marked, NULL),
 *     mtkx_otherwise(keep, NULL),
 *   };
 *   mtkx_match_table table = mtkx_match_table_make(clauses, 2);
 *   enum state next;
 *   mtkx_match_int(&table, UNMARKED, NULL, &next);   // next == MARKED
 * @endcode
 */
#ifndef MEMTKX_DSL_MATCH_H
#define MEMTKX_DSL_MATCH_H

#include <stddef.h>
#include <stdint.h>

#include "memtkx/core/base.h"

MTKX_BEGIN_DECLS

/** The key a table is dispatched on: an integer or a string. */
typedef struct mtkx_match_key {
  bool is_string;  /**< true for mtkx_match_string keys */
  intmax_t value;  /**< integer key (0 for string keys) */
  const char *text;  /**< string key (NULL for integer keys) */
  size_t length;     /**< string key length */
} mtkx_match_key;

/**
 * @brief Clause handler: receives the key, the caller's extra arguments
 *        and a result slot to fill.
 */
typedef void (*mtkx_match_handler)(mtkx_match_key key, void *args, void *result,
                                   void *ctx);

/** Clause kinds. */
typedef enum mtkx_match_clause_kind {
  MTKX_MATCH_WHEN = 0,          /**< integer equality (C++ `when<Value>`) */
  MTKX_MATCH_WHEN_PATTERN = 1,  /**< string pattern (C++ `when<pattern<"...">{}>`) */
  MTKX_MATCH_OTHERWISE = 2      /**< fallback (C++ `otherwise`) */
} mtkx_match_clause_kind;

/** One clause of a dispatch table. */
typedef struct mtkx_match_clause {
  mtkx_match_clause_kind kind;
  intmax_t value;        /**< MTKX_MATCH_WHEN key */
  const char *pattern;   /**< MTKX_MATCH_WHEN_PATTERN pattern (borrowed) */
  mtkx_match_handler handler;
  void *ctx;
} mtkx_match_clause;

/** Value clause: fires when an integer key equals @p value. */
MTKX_INLINE mtkx_match_clause mtkx_when(intmax_t value, mtkx_match_handler handler,
                                        void *ctx) {
  mtkx_match_clause c;
  c.kind = MTKX_MATCH_WHEN;
  c.value = value;
  c.pattern = NULL;
  c.handler = handler;
  c.ctx = ctx;
  return c;
}

/** Pattern clause: fires when a string key matches @p pattern entirely. */
MTKX_INLINE mtkx_match_clause mtkx_when_pattern(const char *pattern,
                                                mtkx_match_handler handler, void *ctx) {
  mtkx_match_clause c = mtkx_when(0, handler, ctx);
  c.kind = MTKX_MATCH_WHEN_PATTERN;
  c.pattern = pattern;
  return c;
}

/** Fallback clause, used when no other clause fires. */
MTKX_INLINE mtkx_match_clause mtkx_otherwise(mtkx_match_handler handler, void *ctx) {
  mtkx_match_clause c = mtkx_when(0, handler, ctx);
  c.kind = MTKX_MATCH_OTHERWISE;
  return c;
}

/** A table over caller-owned clauses (C++: `MatchTable`). */
typedef struct mtkx_match_table {
  const mtkx_match_clause *clauses;
  size_t count;
} mtkx_match_table;

/** Builds a table view of @p count clauses. */
MTKX_INLINE mtkx_match_table mtkx_match_table_make(const mtkx_match_clause *clauses,
                                                   size_t count) {
  mtkx_match_table t;
  t.clauses = clauses;
  t.count = count;
  return t;
}

/**
 * @brief Dispatches on an integer key: the first equal `when` clause, else
 *        the first `otherwise` clause. Pattern clauses are ignored.
 * @return false when nothing fired (the C++ table threw in that case).
 */
MTKX_API bool mtkx_match_int(const mtkx_match_table *table, intmax_t key, void *args,
                             void *result);

/**
 * @brief Dispatches on a string key: the first matching pattern clause,
 *        else the first `otherwise`. Integer clauses are ignored.
 * @return false when nothing fired.
 */
MTKX_API bool mtkx_match_string(const mtkx_match_table *table, const char *key,
                                size_t length, void *args, void *result);

/**
 * @brief Tests whether the whole of @p input matches @p pattern.
 *
 * Supported subset: literal characters, `.` (any character), `[abc]`
 * and `[a-z]` classes, `*` (zero or more) and `+` (one or more) after an
 * atom. `^` is ignored. The match is always anchored at both ends.
 */
MTKX_API bool mtkx_pattern_matches(const char *pattern, const char *input);

/** Length-delimited variant of mtkx_pattern_matches(). */
MTKX_API bool mtkx_pattern_matches_n(const char *pattern, size_t pattern_length,
                                     const char *input, size_t input_length);

MTKX_END_DECLS

#endif /* MEMTKX_DSL_MATCH_H */
