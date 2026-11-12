#include "memtkx/plan/plan.h"

const char *mtkx_plan_kind_string(mtkx_plan_kind kind) {
  switch (kind) {
    case MTKX_PLAN_MARK_SWEEP:
      return "mark-sweep";
    case MTKX_PLAN_SEMI_SPACE:
      return "semi-space";
    case MTKX_PLAN_IMMIX:
      return "immix";
    case MTKX_PLAN_GENERATIONAL:
      return "generational";
  }
  return "unknown";
}

mtkx_plan_kind mtkx_plan_get_kind(const mtkx_plan *plan) { return plan->ops->kind(plan); }

const char *mtkx_plan_name(const mtkx_plan *plan) { return plan->ops->name(plan); }

mtkx_alloc_result mtkx_plan_allocate(mtkx_plan *plan, size_t bytes, size_t alignment) {
  if (plan->ops->allocate == NULL) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
  }
  return plan->ops->allocate(plan, bytes, alignment);
}

mtkx_plan_stats_result mtkx_plan_collect(mtkx_plan *plan, const mtkx_address *roots,
                                         size_t root_count, mtkx_trace trace) {
  if (plan->ops->collect_impl == NULL) {
    return mtkx_plan_stats_result_from_err(MTKX_GC_PLAN_UNAVAILABLE);
  }
  return plan->ops->collect_impl(plan, roots, root_count, trace);
}

void mtkx_plan_destroy(mtkx_plan *plan) {
  if (plan != NULL && plan->ops != NULL && plan->ops->destroy != NULL) {
    plan->ops->destroy(plan);
  }
}
