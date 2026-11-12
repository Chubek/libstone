/* Port of tests/test_plans.cpp plus the guide's graph scenarios. */
#include <memtkx/memtkx.h>

#include "test_support.h"

typedef struct node {
  mtkx_address next;
  int value;
} node;

static bool trace_node(mtkx_address object, mtkx_address_vec *children, void *ctx) {
  const node *n = mtkx_from_address(const node, object);
  (void)ctx;
  return n->next == 0 || mtkx_address_vec_push(children, n->next) == MTKX_OK;
}

static bool failing_trace(mtkx_address object, mtkx_address_vec *children, void *ctx) {
  (void)object, (void)children, (void)ctx;
  return false;
}

static mtkx_address make_node(mtkx_mark_sweep_plan *heap, int value) {
  mtkx_alloc_result a = mtkx_mark_sweep_plan_allocate(heap, sizeof(node), MTKX_ALIGNOF(node));
  node *n = mtkx_from_address(node, mtkx_alloc_result_unwrap(a));
  n->next = 0;
  n->value = value;
  return a.value;
}

int main(void) {
  const mtkx_trace empty = mtkx_trace_none();

  /* --- mark-sweep --- */
  {
    mtkx_mark_sweep_plan *mark_sweep = mtkx_mark_sweep_plan_create(4096);
    mtkx_alloc_result live, dead;
    mtkx_plan_stats_result stats;
    CHECK(mark_sweep != NULL);
    live = mtkx_mark_sweep_plan_allocate(mark_sweep, 64, 8);
    dead = mtkx_mark_sweep_plan_allocate(mark_sweep, 64, 8);
    CHECK(mtkx_alloc_result_is_ok(live));
    CHECK(mtkx_alloc_result_is_ok(dead));
    CHECK(mtkx_mark_sweep_plan_live_objects(mark_sweep) == 2);
    CHECK(mtkx_mark_sweep_plan_live_bytes(mark_sweep) == 128);
    stats = mtkx_mark_sweep_plan_collect(mark_sweep, &live.value, 1, empty);
    CHECK(mtkx_plan_stats_result_is_ok(stats));
    CHECK(mtkx_plan_stats_result_unwrap(stats).live_objects == 1);
    CHECK(stats.value.freed_objects == 1);
    CHECK(stats.value.freed_bytes == 64);
    CHECK(stats.value.collection_count == 1);
    CHECK(mtkx_mark_sweep_plan_mark(mark_sweep, live.value));
    CHECK(mtkx_mark_sweep_plan_is_marked(mark_sweep, live.value));
    CHECK(!mtkx_mark_sweep_plan_mark(mark_sweep, dead.value));
    mtkx_mark_sweep_plan_destroy(mark_sweep);
  }

  /* --- semi-space --- */
  {
    mtkx_semi_space_plan *semi = mtkx_semi_space_plan_create(2048);
    mtkx_alloc_result semi_live, semi_dead;
    mtkx_plan_stats_result stats;
    CHECK(semi != NULL);
    semi_live = mtkx_semi_space_plan_allocate(semi, 128, 8);
    semi_dead = mtkx_semi_space_plan_allocate(semi, 64, 8);
    CHECK(semi_live.ok && semi_dead.ok);
    memset(mtkx_from_address(void, semi_live.value), 0xAB, 128);
    stats = mtkx_semi_space_plan_collect(semi, &semi_live.value, 1, empty);
    CHECK(stats.ok);
    CHECK(stats.value.live_objects == 1);
    CHECK(stats.value.live_bytes == 128);
    CHECK(mtkx_semi_space_plan_live_objects(semi) == 1);
    mtkx_semi_space_plan_destroy(semi);
  }

  /* --- immix --- */
  {
    mtkx_immix_plan *immix = mtkx_immix_plan_create(MTKX_IMMIX_BLOCK_BYTES * 2);
    mtkx_alloc_result immix_live, immix_dead;
    mtkx_plan_stats_result stats;
    CHECK(immix != NULL);
    immix_live = mtkx_immix_plan_allocate(immix, 512, MTKX_IMMIX_LINE_BYTES);
    immix_dead = mtkx_immix_plan_allocate(immix, 256, MTKX_IMMIX_LINE_BYTES);
    CHECK(immix_live.ok && immix_dead.ok);
    stats = mtkx_immix_plan_collect(immix, &immix_live.value, 1, empty);
    CHECK(stats.ok);
    CHECK(stats.value.live_objects == 1);
    CHECK(stats.value.live_bytes == 512);
    CHECK(mtkx_immix_space_marked_bytes(mtkx_immix_plan_space(immix)) >= 512);
    mtkx_immix_plan_destroy(immix);
  }

  /* --- generational --- */
  {
    mtkx_generational_plan *gen = mtkx_generational_plan_create(1024, 4096);
    mtkx_alloc_result young_live, young_dead;
    mtkx_plan_stats_result stats;
    CHECK(gen != NULL);
    young_live = mtkx_generational_plan_allocate(gen, 64, 8);
    young_dead = mtkx_generational_plan_allocate(gen, 64, 8);
    CHECK(young_live.ok && young_dead.ok);
    CHECK(mtkx_generational_plan_nursery_live_bytes(gen) == 128);
    mtkx_generational_plan_remember(gen, 0); /* outside the mature region: ignored */
    stats = mtkx_generational_plan_minor_collect(gen, &young_live.value, 1, empty);
    CHECK(stats.ok);
    CHECK(stats.value.live_objects == 1);
    CHECK(mtkx_generational_plan_nursery_live_bytes(gen) == 0);
    CHECK(mtkx_generational_plan_dirty_cards(gen) == 0);
    mtkx_generational_plan_destroy(gen);
  }

  /* promotion failure reports PLAN_UNAVAILABLE */
  {
    mtkx_generational_plan *gen = mtkx_generational_plan_create(1024, 32);
    mtkx_alloc_result big = mtkx_generational_plan_allocate(gen, 512, 8);
    CHECK(gen != NULL && big.ok);
    CHECK(mtkx_generational_plan_collect(gen, &big.value, 1, empty).error ==
          MTKX_GC_PLAN_UNAVAILABLE);
    mtkx_generational_plan_destroy(gen);
  }

  /* --- guide 7.2: a two-node graph through the polymorphic interface --- */
  {
    mtkx_mark_sweep_plan *heap = mtkx_mark_sweep_plan_create(4096);
    mtkx_plan *plan = mtkx_mark_sweep_plan_as_plan(heap);
    const mtkx_trace trace = mtkx_trace_make(trace_node, NULL);
    mtkx_address root, child;
    mtkx_plan_stats_result r;

    CHECK(mtkx_plan_get_kind(plan) == MTKX_PLAN_MARK_SWEEP);
    CHECK_STR_EQ(mtkx_plan_name(plan), "MarkSweep");
    CHECK_STR_EQ(mtkx_plan_kind_string(MTKX_PLAN_GENERATIONAL), "generational");
    root = make_node(heap, 10);
    child = make_node(heap, 20);
    (void)make_node(heap, 30);
    mtkx_from_address(node, root)->next = child;

    r = mtkx_plan_collect(plan, &root, 1, trace);
    CHECK(r.ok && r.value.live_objects == 2 && r.value.freed_objects == 1);
    CHECK(mtkx_from_address(node, child)->value == 20);

    /* cycle: root <-> child, collected only once unreachable */
    mtkx_from_address(node, child)->next = root;
    r = mtkx_plan_collect(plan, &root, 1, trace);
    CHECK(r.ok && r.value.live_objects == 2 && r.value.freed_objects == 0);
    r = mtkx_plan_collect(plan, NULL, 0, trace);
    CHECK(r.ok && r.value.live_objects == 0 && r.value.freed_objects == 2);
    CHECK(mtkx_mark_sweep_plan_live_objects(heap) == 0);

    /* failing traces are reported, not ignored */
    root = make_node(heap, 1);
    CHECK(mtkx_plan_collect(plan, &root, 1, mtkx_trace_make(failing_trace, NULL)).error ==
          MTKX_GC_INVALID_TRACE);
    CHECK(mtkx_mark_sweep_plan_live_objects(heap) == 1);
    mtkx_plan_destroy(plan);
  }

  /* --- every plan works through mtkx_plan --- */
  {
    mtkx_plan *plans[4];
    size_t i;
    plans[0] = mtkx_mark_sweep_plan_as_plan(mtkx_mark_sweep_plan_create(4096));
    plans[1] = mtkx_semi_space_plan_as_plan(mtkx_semi_space_plan_create(4096));
    plans[2] = mtkx_immix_plan_as_plan(mtkx_immix_plan_create(MTKX_IMMIX_BLOCK_BYTES));
    plans[3] = mtkx_generational_plan_as_plan(mtkx_generational_plan_create(4096, 4096));
    for (i = 0; i < 4; ++i) {
      mtkx_alloc_result a = mtkx_plan_allocate(plans[i], 48, 16);
      mtkx_plan_stats_result r;
      CHECK(a.ok);
      r = mtkx_plan_collect(plans[i], &a.value, 1, empty);
      CHECK(r.ok && r.value.live_objects == 1 && r.value.live_bytes == 48);
      CHECK(mtkx_plan_get_kind(plans[i]) == (mtkx_plan_kind)i);
      mtkx_plan_destroy(plans[i]);
    }
  }
  return 0;
}
