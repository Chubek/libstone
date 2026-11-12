/* Guide chapter 13: a quota space plugin and an observed plan plugin in C. */
#include <memtkx/memtkx.h>

#include "test_support.h"

/* ---------------- quota space ---------------- */

typedef struct quota_space {
  mtkx_space base; /* must be first */
  mtkx_bump_allocator allocator;
  size_t quota;
  size_t charged;
} quota_space;

static mtkx_alloc_result quota_allocate(mtkx_space *space, size_t bytes, size_t alignment) {
  quota_space *q = (quota_space *)space;
  mtkx_alloc_result result;
  if (bytes > q->quota - q->charged) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_OUT_OF_MEMORY);
  }
  result = mtkx_bump_allocator_allocate(&q->allocator, bytes, alignment);
  if (result.ok) {
    q->charged += bytes;
    mtkx_space_record_allocation(space, bytes);
  }
  return result;
}

static const mtkx_space_ops quota_ops = {quota_allocate, NULL};

static mtkx_status quota_space_init(quota_space *q, const char *name, mtkx_address start,
                                    mtkx_address end, size_t quota) {
  mtkx_bump_allocator_init(&q->allocator, start, end);
  q->quota = quota;
  q->charged = 0;
  return mtkx_space_init(&q->base, &quota_ops, name, MTKX_SPACE_BUMP_POINTER,
                         mtkx_range(start, end), MTKX_MAX_ALIGN);
}

static void quota_space_reset(quota_space *q) {
  mtkx_bump_allocator_reset(&q->allocator);
  q->charged = 0;
  mtkx_space_metrics_reset(&q->base.metrics);
}

/* ---------------- observed plan ---------------- */

typedef void (*observer_fn)(const mtkx_plan_stats *stats, void *ctx);

typedef struct observed_plan {
  mtkx_plan base; /* must be first */
  mtkx_mark_sweep_plan *inner;
  observer_fn observer;
  void *observer_ctx;
} observed_plan;

static mtkx_plan_kind observed_kind(const mtkx_plan *p) {
  (void)p;
  return MTKX_PLAN_MARK_SWEEP;
}
static const char *observed_name(const mtkx_plan *p) {
  (void)p;
  return "ObservedMarkSweep";
}
static mtkx_alloc_result observed_allocate(mtkx_plan *p, size_t bytes, size_t alignment) {
  return mtkx_mark_sweep_plan_allocate(((observed_plan *)p)->inner, bytes, alignment);
}
static mtkx_plan_stats_result observed_collect(mtkx_plan *p, const mtkx_address *roots,
                                               size_t n, mtkx_trace trace) {
  observed_plan *o = (observed_plan *)p;
  mtkx_plan_stats_result r = mtkx_mark_sweep_plan_collect(o->inner, roots, n, trace);
  if (r.ok && o->observer != NULL) {
    o->observer(&r.value, o->observer_ctx);
  }
  return r;
}
static void observed_destroy(mtkx_plan *p) {
  mtkx_mark_sweep_plan_destroy(((observed_plan *)p)->inner);
}

static const mtkx_plan_ops observed_ops = {observed_kind, observed_name, observed_allocate,
                                           observed_collect, observed_destroy};

static void count_observation(const mtkx_plan_stats *stats, void *ctx) {
  size_t *observations = (size_t *)ctx;
  ++*observations;
  CHECK(stats->collection_count == *observations);
}

static double storage[4096 / sizeof(double)];

int main(void) {
  {
    quota_space space;
    const mtkx_address start = mtkx_as_address(storage);
    CHECK(quota_space_init(&space, "quota", start, start + sizeof storage, 128) == MTKX_OK);
    CHECK(mtkx_space_allocate(&space.base, 96, 16).ok);
    CHECK(!mtkx_space_allocate(&space.base, 64, 16).ok);
    CHECK(space.charged == 96);
    CHECK(mtkx_space_metrics_of(&space.base)->total_allocations == 1);
    CHECK(mtkx_space_allocate(&space.base, 1, 3).error == MTKX_ALLOC_MISALIGNED);
    quota_space_reset(&space);
    CHECK(space.charged == 0 && mtkx_space_metrics_of(&space.base)->live_bytes == 0);
    mtkx_space_destroy(&space.base);
  }
  {
    size_t observations = 0;
    observed_plan heap;
    mtkx_plan *plan = &heap.base;
    mtkx_alloc_result allocated;
    heap.base.ops = &observed_ops;
    heap.inner = mtkx_mark_sweep_plan_create(4096);
    heap.observer = count_observation;
    heap.observer_ctx = &observations;
    CHECK(heap.inner != NULL);
    allocated = mtkx_plan_allocate(plan, 64, 16);
    CHECK(allocated.ok);
    CHECK(mtkx_plan_collect(plan, &allocated.value, 1, mtkx_trace_none()).ok);
    CHECK(observations == 1);
    CHECK(mtkx_plan_collect(plan, NULL, 0, mtkx_trace_none()).ok);
    CHECK(observations == 2);
    CHECK(mtkx_plan_get_kind(plan) == MTKX_PLAN_MARK_SWEEP);
    CHECK_STR_EQ(mtkx_plan_name(plan), "ObservedMarkSweep");
    mtkx_plan_destroy(plan); /* releases the inner plan; `heap` itself is on the stack */
  }
  return 0;
}
