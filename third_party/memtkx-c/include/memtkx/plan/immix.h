/**
 * @file memtkx/plan/immix.h
 * @brief Line-marking Immix collection plan.
 *
 * Internal bookkeeping failures during collection are reported as
 * MTKX_GC_PLAN_UNAVAILABLE; a trace callback returning false as
 * MTKX_GC_INVALID_TRACE.
 */
#ifndef MEMTKX_PLAN_IMMIX_H
#define MEMTKX_PLAN_IMMIX_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/plan/plan.h"
#include "memtkx/space/immix_space.h"

MTKX_BEGIN_DECLS

/** Line-marking plan over an owned ImmixSpace (C++ `ImmixPlan`). */
typedef struct mtkx_immix_plan mtkx_immix_plan;

/** Creates a plan with an Immix space of at least @p heap_bytes (NULL on OOM). */
MTKX_API mtkx_immix_plan *mtkx_immix_plan_create(size_t heap_bytes);
/** Destroys the plan. */
MTKX_API void mtkx_immix_plan_destroy(mtkx_immix_plan *plan);
/** @return the plan's polymorphic base. */
MTKX_API mtkx_plan *mtkx_immix_plan_as_plan(mtkx_immix_plan *plan);
/** Allocates and tracks an object. */
MTKX_API mtkx_alloc_result mtkx_immix_plan_allocate(mtkx_immix_plan *plan, size_t bytes,
                                                    size_t alignment);
/** Marks a tracked object and its lines; false for unknown addresses. */
MTKX_API bool mtkx_immix_plan_mark(mtkx_immix_plan *plan, mtkx_address object);
/** Marks reachable objects' lines. Does not sweep or recycle lines. */
MTKX_API mtkx_plan_stats_result mtkx_immix_plan_collect(mtkx_immix_plan *plan,
                                                        const mtkx_address *roots,
                                                        size_t root_count, mtkx_trace trace);
/** @return the plan's space, for observation. */
MTKX_API const mtkx_immix_space *mtkx_immix_plan_space(const mtkx_immix_plan *plan);

MTKX_END_DECLS

#endif /* MEMTKX_PLAN_IMMIX_H */
