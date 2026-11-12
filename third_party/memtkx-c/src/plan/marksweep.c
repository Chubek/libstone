#include "memtkx/plan/marksweep.h"

#include "memtkx/space/free_list.h"
#include "plan/plan_internal.h"

struct mtkx_mark_sweep_plan {
  mtkx_plan base;
  void *storage;
  size_t storage_bytes;
  mtkx_free_list_allocator allocator;
  mtkx__addr_map objects; /* address -> requested size */
  mtkx__addr_map marked;  /* set */
  size_t collection_count;
};

static mtkx_plan_kind mtkx__ms_kind(const mtkx_plan *p) {
  (void)p;
  return MTKX_PLAN_MARK_SWEEP;
}
static const char *mtkx__ms_name(const mtkx_plan *p) {
  (void)p;
  return "MarkSweep";
}
static mtkx_alloc_result mtkx__ms_allocate(mtkx_plan *p, size_t bytes, size_t alignment) {
  return mtkx_mark_sweep_plan_allocate((mtkx_mark_sweep_plan *)p, bytes, alignment);
}
static mtkx_plan_stats_result mtkx__ms_collect(mtkx_plan *p, const mtkx_address *roots,
                                               size_t n, mtkx_trace trace) {
  return mtkx_mark_sweep_plan_collect((mtkx_mark_sweep_plan *)p, roots, n, trace);
}
static void mtkx__ms_destroy(mtkx_plan *p) {
  mtkx_mark_sweep_plan_destroy((mtkx_mark_sweep_plan *)p);
}

static const mtkx_plan_ops mtkx__ms_ops = {mtkx__ms_kind, mtkx__ms_name, mtkx__ms_allocate,
                                           mtkx__ms_collect, mtkx__ms_destroy};

mtkx_mark_sweep_plan *mtkx_mark_sweep_plan_create(size_t heap_bytes) {
  mtkx_mark_sweep_plan *plan = (mtkx_mark_sweep_plan *)mtkx_calloc(1, sizeof *plan);
  mtkx_address base;
  if (plan == NULL) {
    return NULL;
  }
  plan->base.ops = &mtkx__ms_ops;
  mtkx__addr_map_init(&plan->objects);
  mtkx__addr_map_init(&plan->marked);
  if (!mtkx__alloc_storage(heap_bytes, &plan->storage)) {
    mtkx_free(plan);
    return NULL;
  }
  plan->storage_bytes = heap_bytes;
  base = mtkx_as_address(plan->storage);
  if (mtkx_free_list_allocator_init(&plan->allocator, base, base + heap_bytes) != MTKX_OK) {
    mtkx_mark_sweep_plan_destroy(plan);
    return NULL;
  }
  return plan;
}

void mtkx_mark_sweep_plan_destroy(mtkx_mark_sweep_plan *plan) {
  if (plan == NULL) {
    return;
  }
  mtkx_free_list_allocator_destroy(&plan->allocator);
  mtkx__addr_map_destroy(&plan->objects);
  mtkx__addr_map_destroy(&plan->marked);
  mtkx_free(plan->storage);
  mtkx_free(plan);
}

mtkx_plan *mtkx_mark_sweep_plan_as_plan(mtkx_mark_sweep_plan *plan) { return &plan->base; }

mtkx_alloc_result mtkx_mark_sweep_plan_allocate(mtkx_mark_sweep_plan *plan, size_t bytes,
                                                size_t alignment) {
  mtkx_alloc_result result = mtkx_free_list_allocator_allocate(&plan->allocator, bytes,
                                                               alignment);
  if (result.ok &&
      mtkx__addr_map_emplace(&plan->objects, result.value, bytes, NULL) != MTKX_OK) {
    (void)mtkx_free_list_allocator_free(&plan->allocator, result.value, bytes);
    return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
  }
  return result;
}

bool mtkx_mark_sweep_plan_mark(mtkx_mark_sweep_plan *plan, mtkx_address object) {
  if (!mtkx__addr_map_contains(&plan->objects, object)) {
    return false;
  }
  return mtkx__addr_map_put(&plan->marked, object, 1) == MTKX_OK;
}

bool mtkx_mark_sweep_plan_is_marked(const mtkx_mark_sweep_plan *plan, mtkx_address object) {
  return mtkx__addr_map_contains(&plan->marked, object);
}

size_t mtkx_mark_sweep_plan_live_objects(const mtkx_mark_sweep_plan *plan) {
  return plan->objects.count;
}

size_t mtkx_mark_sweep_plan_live_bytes(const mtkx_mark_sweep_plan *plan) {
  size_t total = 0;
  size_t i;
  for (i = 0; i < plan->objects.capacity; ++i) {
    if (plan->objects.used[i]) {
      total += (size_t)plan->objects.values[i];
    }
  }
  return total;
}

bool mtkx_mark_sweep_plan_contains_object(const mtkx_mark_sweep_plan *plan,
                                          mtkx_address object) {
  return mtkx__addr_map_contains(&plan->objects, object);
}

mtkx_plan_stats_result mtkx_mark_sweep_plan_collect(mtkx_mark_sweep_plan *plan,
                                                    const mtkx_address *roots,
                                                    size_t root_count, mtkx_trace trace) {
  mtkx_address_vec work = MTKX_ADDRESS_VEC_INIT;
  mtkx_address_vec dead = MTKX_ADDRESS_VEC_INIT;
  mtkx_plan_stats stats = mtkx__stats_zero();
  mtkx_gc_error error = MTKX_GC_PLAN_UNAVAILABLE;
  size_t i;

  mtkx__addr_map_clear(&plan->marked);
  if (!mtkx__seed_roots(&work, roots, root_count)) {
    goto fail;
  }

  /* mark */
  while (!mtkx_address_vec_empty(&work)) {
    const mtkx_address object = mtkx_address_vec_pop(&work);
    bool inserted = false;
    if (!mtkx__addr_map_contains(&plan->objects, object)) {
      continue;
    }
    if (mtkx__addr_map_emplace(&plan->marked, object, 1, &inserted) != MTKX_OK) {
      goto fail;
    }
    if (!inserted) {
      continue;
    }
    if (!mtkx__trace_into(trace, object, &work)) {
      error = MTKX_GC_INVALID_TRACE;
      goto fail;
    }
  }

  /* sweep */
  stats.live_objects = plan->marked.count;
  for (i = 0; i < plan->objects.capacity; ++i) {
    mtkx_address address;
    size_t bytes;
    if (!plan->objects.used[i]) {
      continue;
    }
    address = plan->objects.keys[i];
    bytes = (size_t)plan->objects.values[i];
    if (mtkx__addr_map_contains(&plan->marked, address)) {
      stats.live_bytes += bytes;
    } else {
      if (mtkx_address_vec_push(&dead, address) != MTKX_OK) {
        goto fail;
      }
      stats.freed_objects += 1;
      stats.freed_bytes += bytes;
    }
  }
  for (i = 0; i < dead.size; ++i) {
    uintptr_t bytes = 0;
    (void)mtkx__addr_map_get(&plan->objects, dead.data[i], &bytes);
    (void)mtkx_free_list_allocator_free(&plan->allocator, dead.data[i], (size_t)bytes);
    (void)mtkx__addr_map_remove(&plan->objects, dead.data[i]);
  }

  mtkx__addr_map_clear(&plan->marked);
  ++plan->collection_count;
  stats.collection_count = plan->collection_count;
  mtkx_address_vec_destroy(&work);
  mtkx_address_vec_destroy(&dead);
  return mtkx_plan_stats_result_from_ok(stats);

fail:
  mtkx__addr_map_clear(&plan->marked);
  mtkx_address_vec_destroy(&work);
  mtkx_address_vec_destroy(&dead);
  return mtkx_plan_stats_result_from_err(error);
}
