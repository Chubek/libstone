/**
 * @file memtkx/plan/plan.h
 * @brief The collection-plan interface: kinds, statistics, trace
 *        callbacks and a vtable base (the C counterpart of the CRTP
 *        `PlanBase<Derived>`).
 *
 * Every built-in plan is an opaque object whose `mtkx_plan` base can be
 * obtained with its `*_as_plan()` function, so plans can be selected at
 * run time and used through mtkx_plan_allocate() / mtkx_plan_collect().
 * Application plans embed ::mtkx_plan as their first member and supply an
 * ::mtkx_plan_ops table.
 */
#ifndef MEMTKX_PLAN_PLAN_H
#define MEMTKX_PLAN_PLAN_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/core/vec.h"
#include "memtkx/dsl/result.h"

MTKX_BEGIN_DECLS

/** The built-in plan algorithms (a closed set, as in C++). */
typedef enum mtkx_plan_kind {
  MTKX_PLAN_MARK_SWEEP = 0,
  MTKX_PLAN_SEMI_SPACE = 1,
  MTKX_PLAN_IMMIX = 2,
  MTKX_PLAN_GENERATIONAL = 3
} mtkx_plan_kind;

/** @return "mark-sweep", "semi-space", "immix" or "generational". */
MTKX_API const char *mtkx_plan_kind_string(mtkx_plan_kind kind);

/** Statistics reported by one collection. Freed counters: mark-sweep only. */
typedef struct mtkx_plan_stats {
  size_t live_objects;
  size_t live_bytes;
  size_t freed_objects;
  size_t freed_bytes;
  size_t collection_count;
} mtkx_plan_stats;

/** Result of a collection (C++ `GcResult<PlanStats>`). */
MTKX_DEFINE_RESULT(mtkx_plan_stats_result, mtkx_plan_stats, mtkx_gc_error);

/**
 * @brief Object scanner (C++ `TraceFn`): append the addresses of the
 *        objects referenced by @p object to @p children.
 *
 * Return false if the object cannot be scanned (including a failed
 * append); the collection then stops with MTKX_GC_INVALID_TRACE.
 */
typedef bool (*mtkx_trace_fn)(mtkx_address object, mtkx_address_vec *children, void *ctx);

/** A trace callback and its context. A NULL fn skips child scanning. */
typedef struct mtkx_trace {
  mtkx_trace_fn fn;
  void *ctx;
} mtkx_trace;

/** Builds a trace. */
MTKX_INLINE mtkx_trace mtkx_trace_make(mtkx_trace_fn fn, void *ctx) {
  mtkx_trace t;
  t.fn = fn;
  t.ctx = ctx;
  return t;
}

/** A trace that reports no children (C++ empty `TraceFn`). */
MTKX_INLINE mtkx_trace mtkx_trace_none(void) { return mtkx_trace_make(NULL, NULL); }

typedef struct mtkx_plan mtkx_plan;

/** Behaviour supplied by a plan implementation. */
typedef struct mtkx_plan_ops {
  /** C++ `kind_value()`. */
  mtkx_plan_kind (*kind)(const mtkx_plan *plan);
  /** C++ `name_value()`, e.g. "MarkSweep". */
  const char *(*name)(const mtkx_plan *plan);
  /** Allocation (may be NULL: allocation then fails with ALLOCATION_FAILED). */
  mtkx_alloc_result (*allocate)(mtkx_plan *plan, size_t bytes, size_t alignment);
  /** C++ `collect_impl(roots, trace)`. */
  mtkx_plan_stats_result (*collect_impl)(mtkx_plan *plan, const mtkx_address *roots,
                                         size_t root_count, mtkx_trace trace);
  /** Releases the plan object itself (may be NULL for caller-owned plans). */
  void (*destroy)(mtkx_plan *plan);
} mtkx_plan_ops;

/** The base of every plan. Embed it as the first member. */
struct mtkx_plan {
  const mtkx_plan_ops *ops;
};

/** @return the plan's kind. */
MTKX_API mtkx_plan_kind mtkx_plan_get_kind(const mtkx_plan *plan);
/** @return the plan's display name. */
MTKX_API const char *mtkx_plan_name(const mtkx_plan *plan);
/** Allocates through the plan. */
MTKX_API mtkx_alloc_result mtkx_plan_allocate(mtkx_plan *plan, size_t bytes,
                                              size_t alignment);
/**
 * @brief Collects from @p root_count roots using @p trace
 *        (C++ `PlanBase::collect`).
 */
MTKX_API mtkx_plan_stats_result mtkx_plan_collect(mtkx_plan *plan, const mtkx_address *roots,
                                                  size_t root_count, mtkx_trace trace);
/** Destroys the plan through `ops->destroy` (NULL is ignored). */
MTKX_API void mtkx_plan_destroy(mtkx_plan *plan);

MTKX_END_DECLS

#endif /* MEMTKX_PLAN_PLAN_H */
