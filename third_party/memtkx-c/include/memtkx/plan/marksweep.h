/**
 * @file memtkx/plan/marksweep.h
 * @brief Non-moving mark-sweep collection plan.
 *
 * Internal bookkeeping failures during collection are reported as
 * MTKX_GC_PLAN_UNAVAILABLE; a trace callback returning false as
 * MTKX_GC_INVALID_TRACE.
 */
#ifndef MEMTKX_PLAN_MARKSWEEP_H
#define MEMTKX_PLAN_MARKSWEEP_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/plan/plan.h"

MTKX_BEGIN_DECLS

/** Non-moving mark-sweep over an owned free-list heap (C++ `MarkSweepPlan`). */
typedef struct mtkx_mark_sweep_plan mtkx_mark_sweep_plan;

/** Creates a plan owning @p heap_bytes of storage (NULL on OOM). */
MTKX_API mtkx_mark_sweep_plan *mtkx_mark_sweep_plan_create(size_t heap_bytes);
/** Destroys the plan and its storage. */
MTKX_API void mtkx_mark_sweep_plan_destroy(mtkx_mark_sweep_plan *plan);
/** @return the plan's polymorphic base. */
MTKX_API mtkx_plan *mtkx_mark_sweep_plan_as_plan(mtkx_mark_sweep_plan *plan);
/** Allocates and tracks an object. */
MTKX_API mtkx_alloc_result mtkx_mark_sweep_plan_allocate(mtkx_mark_sweep_plan *plan,
                                                         size_t bytes, size_t alignment);
/** Marks a tracked object; false for unknown addresses. */
MTKX_API bool mtkx_mark_sweep_plan_mark(mtkx_mark_sweep_plan *plan, mtkx_address object);
/** @return whether @p object is marked. */
MTKX_API bool mtkx_mark_sweep_plan_is_marked(const mtkx_mark_sweep_plan *plan,
                                             mtkx_address object);
/** @return the number of tracked objects. */
MTKX_API size_t mtkx_mark_sweep_plan_live_objects(const mtkx_mark_sweep_plan *plan);
/** @return the requested bytes of tracked objects. */
MTKX_API size_t mtkx_mark_sweep_plan_live_bytes(const mtkx_mark_sweep_plan *plan);
/** @return whether @p object is a tracked object start. */
MTKX_API bool mtkx_mark_sweep_plan_contains_object(const mtkx_mark_sweep_plan *plan,
                                                   mtkx_address object);
/**
 * @brief Marks from roots, then frees every unmarked object back to the
 *        free list. Survivors keep their addresses. Marks are cleared
 *        before and after.
 */
MTKX_API mtkx_plan_stats_result mtkx_mark_sweep_plan_collect(mtkx_mark_sweep_plan *plan,
                                                             const mtkx_address *roots,
                                                             size_t root_count,
                                                             mtkx_trace trace);

MTKX_END_DECLS

#endif /* MEMTKX_PLAN_MARKSWEEP_H */
