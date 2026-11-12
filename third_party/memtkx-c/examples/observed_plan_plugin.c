/* GUIDE 13.3: a plan plugin written in C. */
#include <memtkx/memtkx.h>

#include <assert.h>
#include <string.h>

typedef struct observed_plan {
  mtkx_plan base; /* first member */
  mtkx_mark_sweep_plan *inner;
  size_t observations;
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
                                               size_t count, mtkx_trace trace) {
  observed_plan *o = (observed_plan *)p;
  mtkx_plan_stats_result r = mtkx_mark_sweep_plan_collect(o->inner, roots, count, trace);
  if (r.ok) {
    ++o->observations;
    assert(r.value.collection_count == o->observations);
  }
  return r;
}
static void observed_destroy(mtkx_plan *p) {
  mtkx_mark_sweep_plan_destroy(((observed_plan *)p)->inner);
}

static const mtkx_plan_ops observed_ops = {observed_kind, observed_name, observed_allocate,
                                           observed_collect, observed_destroy};

int main(void) {
  observed_plan heap;
  mtkx_alloc_result allocated;
  mtkx_plan_stats_result collected;
  heap.base.ops = &observed_ops;
  heap.inner = mtkx_mark_sweep_plan_create(4096);
  heap.observations = 0;
  if (heap.inner == NULL) {
    return 1;
  }

  allocated = mtkx_plan_allocate(&heap.base, 64, 16);
  assert(allocated.ok);
  collected = mtkx_plan_collect(&heap.base, &allocated.value, 1, mtkx_trace_none());
  assert(collected.ok && heap.observations == 1);
  assert(mtkx_plan_get_kind(&heap.base) == MTKX_PLAN_MARK_SWEEP);
  assert(strcmp(mtkx_plan_name(&heap.base), "ObservedMarkSweep") == 0);
  mtkx_plan_destroy(&heap.base); /* releases the inner plan */
  return 0;
}
