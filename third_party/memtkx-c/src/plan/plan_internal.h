/*
 * Shared helpers for the built-in plans. Not installed.
 */
#ifndef MEMTKX_PLAN_INTERNAL_H
#define MEMTKX_PLAN_INTERNAL_H

#include "memtkx/plan/plan.h"

#include "internal/addr_map.h"
#include "internal/common.h"

/* Seeds a worklist with roots. */
static inline bool mtkx__seed_roots(mtkx_address_vec *work, const mtkx_address *roots,
                                    size_t root_count) {
  return root_count == 0 || mtkx_address_vec_append(work, roots, root_count) == MTKX_OK;
}

/* Runs the trace callback, appending children to `out`. */
static inline bool mtkx__trace_into(mtkx_trace trace, mtkx_address object,
                                    mtkx_address_vec *out) {
  if (trace.fn == NULL) {
    return true;
  }
  return trace.fn(object, out, trace.ctx);
}

static inline mtkx_plan_stats mtkx__stats_zero(void) {
  mtkx_plan_stats stats;
  memset(&stats, 0, sizeof stats);
  return stats;
}

/* Allocates an owned, zeroed heap buffer like std::vector<std::byte>(n). */
static inline bool mtkx__alloc_storage(size_t bytes, void **out) {
  *out = NULL;
  if (bytes == 0) {
    return true;
  }
  *out = mtkx_calloc(bytes, 1);
  return *out != NULL;
}

#endif /* MEMTKX_PLAN_INTERNAL_H */
