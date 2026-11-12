/**
 * @file memtkx/plan/semispace.h
 * @brief Copying semi-space collection plan.
 *
 * Internal bookkeeping failures during collection are reported as
 * MTKX_GC_PLAN_UNAVAILABLE; a trace callback returning false as
 * MTKX_GC_INVALID_TRACE.
 */
#ifndef MEMTKX_PLAN_SEMISPACE_H
#define MEMTKX_PLAN_SEMISPACE_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/plan/plan.h"

MTKX_BEGIN_DECLS

/** Copying collector over two equal halves (C++ `SemiSpacePlan`). */
typedef struct mtkx_semi_space_plan mtkx_semi_space_plan;

/** Creates a plan owning 2 * @p semi_space_bytes (NULL on OOM). */
MTKX_API mtkx_semi_space_plan *mtkx_semi_space_plan_create(size_t semi_space_bytes);
/** Destroys the plan. */
MTKX_API void mtkx_semi_space_plan_destroy(mtkx_semi_space_plan *plan);
/** @return the plan's polymorphic base. */
MTKX_API mtkx_plan *mtkx_semi_space_plan_as_plan(mtkx_semi_space_plan *plan);
/** Allocates in from-space and tracks the object. */
MTKX_API mtkx_alloc_result mtkx_semi_space_plan_allocate(mtkx_semi_space_plan *plan,
                                                         size_t bytes, size_t alignment);
/** @return the number of tracked objects. */
MTKX_API size_t mtkx_semi_space_plan_live_objects(const mtkx_semi_space_plan *plan);
/**
 * @brief Copies reachable objects to to-space (memcpy, MTKX_MAX_ALIGN
 *        alignment) and swaps halves. Roots and fields are not rewritten
 *        and forwarding is not exposed, exactly as in C++.
 */
MTKX_API mtkx_plan_stats_result mtkx_semi_space_plan_collect(mtkx_semi_space_plan *plan,
                                                             const mtkx_address *roots,
                                                             size_t root_count,
                                                             mtkx_trace trace);

MTKX_END_DECLS

#endif /* MEMTKX_PLAN_SEMISPACE_H */
