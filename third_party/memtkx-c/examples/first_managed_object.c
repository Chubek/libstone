/* README: your first managed object. */
#include <memtkx/memtkx.h>

#include <stdio.h>

typedef struct node {
  mtkx_address next;
  int value;
} node;

static bool trace_node(mtkx_address object, mtkx_address_vec *children, void *ctx) {
  const node *current = mtkx_from_address(const node, object);
  (void)ctx;
  return current->next == 0 || mtkx_address_vec_push(children, current->next) == MTKX_OK;
}

int main(void) {
  mtkx_mark_sweep_plan *heap = mtkx_mark_sweep_plan_create(MTKX_KB(64));
  mtkx_alloc_result allocation;
  mtkx_address root;
  node *n;
  mtkx_plan_stats_result collected;

  if (heap == NULL) {
    return 1;
  }
  allocation = mtkx_mark_sweep_plan_allocate(heap, sizeof(node), MTKX_ALIGNOF(node));
  if (mtkx_alloc_result_is_err(allocation)) {
    fprintf(stderr, "%s\n", mtkx_alloc_error_string(allocation.error));
    mtkx_mark_sweep_plan_destroy(heap);
    return 1;
  }

  root = mtkx_alloc_result_unwrap(allocation);
  n = mtkx_from_address(node, root);
  n->next = 0;
  n->value = 42;

  collected = mtkx_mark_sweep_plan_collect(heap, &root, 1, mtkx_trace_make(trace_node, NULL));
  if (mtkx_plan_stats_result_is_err(collected)) {
    mtkx_mark_sweep_plan_destroy(heap);
    return 2;
  }
  printf("value=%d, live_objects=%zu, collections=%zu\n", n->value,
         collected.value.live_objects, collected.value.collection_count);

  collected = mtkx_mark_sweep_plan_collect(heap, NULL, 0, mtkx_trace_make(trace_node, NULL));
  mtkx_mark_sweep_plan_destroy(heap);
  return collected.ok ? 0 : 3;
}
