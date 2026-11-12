#include "memtkx/plan/semispace.h"

#include <string.h>

#include "memtkx/space/bump_pointer.h"
#include "plan/plan_internal.h"

struct mtkx_semi_space_plan {
  mtkx_plan base;
  void *storage;
  size_t storage_bytes;
  size_t semi_bytes;
  mtkx_bump_allocator from;
  mtkx_bump_allocator to;
  mtkx__addr_map objects; /* address -> requested size */
  size_t collection_count;
};

static mtkx_plan_kind mtkx__ss_kind(const mtkx_plan *p) {
  (void)p;
  return MTKX_PLAN_SEMI_SPACE;
}
static const char *mtkx__ss_name(const mtkx_plan *p) {
  (void)p;
  return "SemiSpace";
}
static mtkx_alloc_result mtkx__ss_allocate(mtkx_plan *p, size_t bytes, size_t alignment) {
  return mtkx_semi_space_plan_allocate((mtkx_semi_space_plan *)p, bytes, alignment);
}
static mtkx_plan_stats_result mtkx__ss_collect(mtkx_plan *p, const mtkx_address *roots,
                                               size_t n, mtkx_trace trace) {
  return mtkx_semi_space_plan_collect((mtkx_semi_space_plan *)p, roots, n, trace);
}
static void mtkx__ss_destroy(mtkx_plan *p) {
  mtkx_semi_space_plan_destroy((mtkx_semi_space_plan *)p);
}

static const mtkx_plan_ops mtkx__ss_ops = {mtkx__ss_kind, mtkx__ss_name, mtkx__ss_allocate,
                                           mtkx__ss_collect, mtkx__ss_destroy};

mtkx_semi_space_plan *mtkx_semi_space_plan_create(size_t semi_space_bytes) {
  mtkx_semi_space_plan *plan;
  mtkx_address base;
  size_t total;
  if (!mtkx__mul_size(semi_space_bytes, 2, &total)) {
    return NULL;
  }
  plan = (mtkx_semi_space_plan *)mtkx_calloc(1, sizeof *plan);
  if (plan == NULL) {
    return NULL;
  }
  plan->base.ops = &mtkx__ss_ops;
  mtkx__addr_map_init(&plan->objects);
  if (!mtkx__alloc_storage(total, &plan->storage)) {
    mtkx_free(plan);
    return NULL;
  }
  plan->storage_bytes = total;
  plan->semi_bytes = semi_space_bytes;
  base = mtkx_as_address(plan->storage);
  mtkx_bump_allocator_init(&plan->from, base, base + semi_space_bytes);
  mtkx_bump_allocator_init(&plan->to, base + semi_space_bytes, base + total);
  return plan;
}

void mtkx_semi_space_plan_destroy(mtkx_semi_space_plan *plan) {
  if (plan == NULL) {
    return;
  }
  mtkx__addr_map_destroy(&plan->objects);
  mtkx_free(plan->storage);
  mtkx_free(plan);
}

mtkx_plan *mtkx_semi_space_plan_as_plan(mtkx_semi_space_plan *plan) { return &plan->base; }

mtkx_alloc_result mtkx_semi_space_plan_allocate(mtkx_semi_space_plan *plan, size_t bytes,
                                                size_t alignment) {
  mtkx_alloc_result result = mtkx_bump_allocator_allocate(&plan->from, bytes, alignment);
  if (result.ok &&
      mtkx__addr_map_emplace(&plan->objects, result.value, bytes, NULL) != MTKX_OK) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
  }
  return result;
}

size_t mtkx_semi_space_plan_live_objects(const mtkx_semi_space_plan *plan) {
  return plan->objects.count;
}

typedef struct mtkx__ss_copy {
  mtkx_semi_space_plan *plan;
  mtkx__addr_map survivors;  /* new address -> size */
  mtkx__addr_map forwarding; /* old address -> new address */
  mtkx_address_vec work;     /* old addresses still to scan */
  bool failed;               /* bookkeeping allocation failed */
} mtkx__ss_copy;

/* Port of the C++ `forward` lambda: returns 0 for unknown objects or when
 * to-space is exhausted (which, as in C++, is not reported as an error). */
static mtkx_address mtkx__ss_forward(mtkx__ss_copy *c, mtkx_address old_address) {
  uintptr_t found;
  uintptr_t bytes;
  mtkx_alloc_result destination;
  if (mtkx__addr_map_get(&c->forwarding, old_address, &found)) {
    return (mtkx_address)found;
  }
  if (!mtkx__addr_map_get(&c->plan->objects, old_address, &bytes)) {
    return 0;
  }
  destination = mtkx_bump_allocator_allocate(&c->plan->to, (size_t)bytes, MTKX_MAX_ALIGN);
  if (!destination.ok) {
    return 0;
  }
  memcpy(mtkx_from_address(void, destination.value), mtkx_from_address(void, old_address),
         (size_t)bytes);
  if (mtkx__addr_map_put(&c->forwarding, old_address, destination.value) != MTKX_OK ||
      mtkx__addr_map_put(&c->survivors, destination.value, bytes) != MTKX_OK ||
      mtkx_address_vec_push(&c->work, old_address) != MTKX_OK) {
    c->failed = true;
  }
  return destination.value;
}

mtkx_plan_stats_result mtkx_semi_space_plan_collect(mtkx_semi_space_plan *plan,
                                                    const mtkx_address *roots,
                                                    size_t root_count, mtkx_trace trace) {
  mtkx__ss_copy c;
  mtkx_address_vec children = MTKX_ADDRESS_VEC_INIT;
  mtkx_plan_stats stats = mtkx__stats_zero();
  mtkx_gc_error error = MTKX_GC_PLAN_UNAVAILABLE;
  mtkx_bump_allocator swap;
  size_t i;

  c.plan = plan;
  mtkx__addr_map_init(&c.survivors);
  mtkx__addr_map_init(&c.forwarding);
  mtkx_address_vec_init(&c.work);
  c.failed = false;

  mtkx_bump_allocator_reset(&plan->to);
  for (i = 0; i < root_count && !c.failed; ++i) {
    (void)mtkx__ss_forward(&c, roots[i]);
  }
  while (!c.failed && !mtkx_address_vec_empty(&c.work)) {
    const mtkx_address old_address = mtkx_address_vec_pop(&c.work);
    if (trace.fn == NULL) {
      continue;
    }
    mtkx_address_vec_clear(&children);
    if (!trace.fn(old_address, &children, trace.ctx)) {
      error = MTKX_GC_INVALID_TRACE;
      goto fail;
    }
    for (i = 0; i < children.size && !c.failed; ++i) {
      (void)mtkx__ss_forward(&c, children.data[i]);
    }
  }
  if (c.failed) {
    goto fail;
  }

  mtkx__addr_map_swap(&plan->objects, &c.survivors);
  swap = plan->from;
  plan->from = plan->to;
  plan->to = swap;
  mtkx_bump_allocator_reset(&plan->to);

  stats.live_objects = plan->objects.count;
  for (i = 0; i < plan->objects.capacity; ++i) {
    if (plan->objects.used[i]) {
      stats.live_bytes += (size_t)plan->objects.values[i];
    }
  }
  ++plan->collection_count;
  stats.collection_count = plan->collection_count;
  mtkx__addr_map_destroy(&c.survivors);
  mtkx__addr_map_destroy(&c.forwarding);
  mtkx_address_vec_destroy(&c.work);
  mtkx_address_vec_destroy(&children);
  return mtkx_plan_stats_result_from_ok(stats);

fail:
  mtkx__addr_map_destroy(&c.survivors);
  mtkx__addr_map_destroy(&c.forwarding);
  mtkx_address_vec_destroy(&c.work);
  mtkx_address_vec_destroy(&children);
  return mtkx_plan_stats_result_from_err(error);
}
