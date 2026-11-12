/**
 * @file memtkx/plan/generational.h
 * @brief Nursery/mature generational collection plan.
 *
 * Internal bookkeeping failures during collection are reported as
 * MTKX_GC_PLAN_UNAVAILABLE; a trace callback returning false as
 * MTKX_GC_INVALID_TRACE.
 */
#ifndef MEMTKX_PLAN_GENERATIONAL_H
#define MEMTKX_PLAN_GENERATIONAL_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/plan/plan.h"

MTKX_BEGIN_DECLS

/** Nursery + mature heaps with a card table (C++ `GenerationalPlan`). */
typedef struct mtkx_generational_plan mtkx_generational_plan;

/** Creates a plan owning both heaps (NULL on OOM). */
MTKX_API mtkx_generational_plan *mtkx_generational_plan_create(size_t nursery_bytes,
                                                               size_t mature_bytes);
/** Destroys the plan. */
MTKX_API void mtkx_generational_plan_destroy(mtkx_generational_plan *plan);
/** @return the plan's polymorphic base. */
MTKX_API mtkx_plan *mtkx_generational_plan_as_plan(mtkx_generational_plan *plan);
/** Allocates in the nursery. */
MTKX_API mtkx_alloc_result mtkx_generational_plan_allocate(mtkx_generational_plan *plan,
                                                           size_t bytes, size_t alignment);
/** Dirties the card of @p slot_address (cards are not scanned as roots). */
MTKX_API void mtkx_generational_plan_remember(mtkx_generational_plan *plan,
                                              mtkx_address slot_address);
/** @return tracked requested bytes whose address lies in the nursery. */
MTKX_API size_t mtkx_generational_plan_nursery_live_bytes(const mtkx_generational_plan *plan);
/** @return the number of dirty cards. */
MTKX_API size_t mtkx_generational_plan_dirty_cards(const mtkx_generational_plan *plan);
/**
 * @brief Copies reachable tracked objects into mature storage, replaces
 *        the object table with the copies, resets the nursery and clears
 *        cards. A failed promotion returns MTKX_GC_PLAN_UNAVAILABLE.
 */
MTKX_API mtkx_plan_stats_result mtkx_generational_plan_minor_collect(
    mtkx_generational_plan *plan, const mtkx_address *roots, size_t root_count,
    mtkx_trace trace);
/** Same as mtkx_generational_plan_minor_collect(). */
MTKX_API mtkx_plan_stats_result mtkx_generational_plan_collect(mtkx_generational_plan *plan,
                                                               const mtkx_address *roots,
                                                               size_t root_count,
                                                               mtkx_trace trace);

MTKX_END_DECLS

#endif /* MEMTKX_PLAN_GENERATIONAL_H */
