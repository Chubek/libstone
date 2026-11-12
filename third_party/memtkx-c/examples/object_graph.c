/* GUIDE 7.2: a two-node graph collected repeatedly. */
#include <memtkx/memtkx.h>

#include <assert.h>
#include <stdlib.h>

typedef struct node {
  mtkx_address next;
  int value;
} node;

static bool trace(mtkx_address address, mtkx_address_vec *children, void *ctx) {
  const node *n = mtkx_from_address(const node, address);
  (void)ctx;
  return n->next == 0 || mtkx_address_vec_push(children, n->next) == MTKX_OK;
}

static mtkx_address make_node(mtkx_mark_sweep_plan *heap, int value) {
  mtkx_alloc_result a = mtkx_mark_sweep_plan_allocate(heap, sizeof(node), MTKX_ALIGNOF(node));
  node *n;
  if (!a.ok) {
    abort();
  }
  n = mtkx_from_address(node, a.value);
  n->next = 0;
  n->value = value;
  return a.value;
}

int main(void) {
  mtkx_mark_sweep_plan *heap = mtkx_mark_sweep_plan_create(4096);
  const mtkx_trace t = mtkx_trace_make(trace, NULL);
  mtkx_address root, child;
  mtkx_plan_stats_result r;

  root = make_node(heap, 10);
  child = make_node(heap, 20);
  (void)make_node(heap, 30);
  mtkx_from_address(node, root)->next = child;

  r = mtkx_mark_sweep_plan_collect(heap, &root, 1, t);
  assert(r.ok && r.value.live_objects == 2 && r.value.freed_objects == 1);
  assert(mtkx_from_address(node, child)->value == 20);

  mtkx_from_address(node, root)->next = 0;
  r = mtkx_mark_sweep_plan_collect(heap, &root, 1, t);
  assert(r.ok && r.value.live_objects == 1 && r.value.freed_objects == 1);

  r = mtkx_mark_sweep_plan_collect(heap, NULL, 0, t);
  assert(r.ok && mtkx_mark_sweep_plan_live_objects(heap) == 0);
  mtkx_mark_sweep_plan_destroy(heap);
  return 0;
}
