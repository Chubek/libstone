#include "memtkx/plan/immix.h"

#include "plan/plan_internal.h"

struct mtkx_immix_plan {
  mtkx_plan base;
  mtkx_immix_space space;
  mtkx__addr_map objects; /* address -> requested size */
  mtkx__addr_map marked;  /* set */
  size_t collection_count;
};

static mtkx_plan_kind mtkx__ix_kind(const mtkx_plan *p) {
  (void)p;
  return MTKX_PLAN_IMMIX;
}
static const char *mtkx__ix_name(const mtkx_plan *p) {
  (void)p;
  return "Immix";
}
static mtkx_alloc_result mtkx__ix_allocate(mtkx_plan *p, size_t bytes, size_t alignment) {
  return mtkx_immix_plan_allocate((mtkx_immix_plan *)p, bytes, alignment);
}
static mtkx_plan_stats_result mtkx__ix_collect(mtkx_plan *p, const mtkx_address *roots,
                                               size_t n, mtkx_trace trace) {
  return mtkx_immix_plan_collect((mtkx_immix_plan *)p, roots, n, trace);
}
static void mtkx__ix_destroy(mtkx_plan *p) { mtkx_immix_plan_destroy((mtkx_immix_plan *)p); }

static const mtkx_plan_ops mtkx__ix_ops = {mtkx__ix_kind, mtkx__ix_name, mtkx__ix_allocate,
                                           mtkx__ix_collect, mtkx__ix_destroy};

mtkx_immix_plan *mtkx_immix_plan_create(size_t heap_bytes) {
  mtkx_immix_plan *plan = (mtkx_immix_plan *)mtkx_calloc(1, sizeof *plan);
  if (plan == NULL) {
    return NULL;
  }
  plan->base.ops = &mtkx__ix_ops;
  mtkx__addr_map_init(&plan->objects);
  mtkx__addr_map_init(&plan->marked);
  if (mtkx_immix_space_init(&plan->space, heap_bytes) != MTKX_OK) {
    mtkx_free(plan);
    return NULL;
  }
  return plan;
}

void mtkx_immix_plan_destroy(mtkx_immix_plan *plan) {
  if (plan == NULL) {
    return;
  }
  mtkx_immix_space_destroy(&plan->space);
  mtkx__addr_map_destroy(&plan->objects);
  mtkx__addr_map_destroy(&plan->marked);
  mtkx_free(plan);
}

mtkx_plan *mtkx_immix_plan_as_plan(mtkx_immix_plan *plan) { return &plan->base; }

mtkx_alloc_result mtkx_immix_plan_allocate(mtkx_immix_plan *plan, size_t bytes,
                                           size_t alignment) {
  mtkx_alloc_result result = mtkx_immix_space_allocate(&plan->space, bytes, alignment);
  if (result.ok &&
      mtkx__addr_map_emplace(&plan->objects, result.value, bytes, NULL) != MTKX_OK) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
  }
  return result;
}

bool mtkx_immix_plan_mark(mtkx_immix_plan *plan, mtkx_address object) {
  uintptr_t bytes;
  if (!mtkx__addr_map_get(&plan->objects, object, &bytes)) {
    return false;
  }
  if (mtkx__addr_map_put(&plan->marked, object, 1) != MTKX_OK) {
    return false;
  }
  return mtkx_immix_space_mark(&plan->space, object, (size_t)bytes);
}

mtkx_plan_stats_result mtkx_immix_plan_collect(mtkx_immix_plan *plan,
                                               const mtkx_address *roots, size_t root_count,
                                               mtkx_trace trace) {
  mtkx_address_vec work = MTKX_ADDRESS_VEC_INIT;
  mtkx_plan_stats stats = mtkx__stats_zero();
  mtkx_gc_error error = MTKX_GC_PLAN_UNAVAILABLE;
  size_t i;

  mtkx__addr_map_clear(&plan->marked);
  mtkx_immix_space_clear_marks(&plan->space);
  if (!mtkx__seed_roots(&work, roots, root_count)) {
    goto fail;
  }
  while (!mtkx_address_vec_empty(&work)) {
    const mtkx_address object = mtkx_address_vec_pop(&work);
    if (!mtkx__addr_map_contains(&plan->objects, object) ||
        mtkx__addr_map_contains(&plan->marked, object)) {
      continue;
    }
    if (!mtkx_immix_plan_mark(plan, object) &&
        !mtkx__addr_map_contains(&plan->marked, object)) {
      goto fail; /* the marked set could not grow */
    }
    if (!mtkx__trace_into(trace, object, &work)) {
      error = MTKX_GC_INVALID_TRACE;
      goto fail;
    }
  }

  stats.live_objects = plan->marked.count;
  for (i = 0; i < plan->marked.capacity; ++i) {
    if (plan->marked.used[i]) {
      uintptr_t bytes = 0;
      (void)mtkx__addr_map_get(&plan->objects, plan->marked.keys[i], &bytes);
      stats.live_bytes += (size_t)bytes;
    }
  }
  ++plan->collection_count;
  stats.collection_count = plan->collection_count;
  mtkx_address_vec_destroy(&work);
  return mtkx_plan_stats_result_from_ok(stats);

fail:
  mtkx_address_vec_destroy(&work);
  return mtkx_plan_stats_result_from_err(error);
}

const mtkx_immix_space *mtkx_immix_plan_space(const mtkx_immix_plan *plan) {
  return &plan->space;
}
