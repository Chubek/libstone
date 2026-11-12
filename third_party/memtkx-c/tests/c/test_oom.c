/*
 * Fault injection: replaces the allocator hooks with one that fails the
 * Nth allocation, then runs a workload touching every subsystem for
 * N = 0, 1, 2, ... until it completes without an injected failure. Every
 * run must fail cleanly (no crash, no leak under ASan/valgrind).
 */
#include <stdlib.h>

#include <memtkx/memtkx.h>

#include "test_support.h"

static long countdown = -1; /* allocations left before failing; <0 = never */
static long live_blocks = 0;
static int injected = 0;

static int should_fail(void) {
  if (countdown < 0) {
    return 0;
  }
  if (countdown == 0) {
    injected = 1;
    return 1;
  }
  --countdown;
  return 0;
}

static void *test_malloc(size_t n) {
  void *p = should_fail() ? NULL : malloc(n);
  live_blocks += p != NULL;
  return p;
}
static void *test_calloc(size_t c, size_t n) {
  void *p = should_fail() ? NULL : calloc(c, n);
  live_blocks += p != NULL;
  return p;
}
static void *test_realloc(void *old, size_t n) {
  void *p;
  if (should_fail()) {
    return NULL;
  }
  p = realloc(old, n);
  if (old == NULL && p != NULL) {
    ++live_blocks;
  }
  return p;
}
static void test_free(void *p) {
  if (p != NULL) {
    --live_blocks;
  }
  free(p);
}

typedef struct node {
  mtkx_address next;
} node;

static bool trace_node(mtkx_address object, mtkx_address_vec *children, void *ctx) {
  const node *n = mtkx_from_address(const node, object);
  (void)ctx;
  return n->next == 0 || mtkx_address_vec_push(children, n->next) == MTKX_OK;
}

static bool digit(char c, void *ctx) {
  (void)ctx;
  return c >= '0' && c <= '9';
}

static void workload(void) {
  /* plans */
  {
    mtkx_plan *plans[4];
    size_t i;
    {
      mtkx_mark_sweep_plan *ms = mtkx_mark_sweep_plan_create(4096);
      mtkx_semi_space_plan *ss = mtkx_semi_space_plan_create(4096);
      mtkx_immix_plan *ix = mtkx_immix_plan_create(MTKX_IMMIX_BLOCK_BYTES);
      mtkx_generational_plan *gen = mtkx_generational_plan_create(4096, 8192);
      plans[0] = ms ? mtkx_mark_sweep_plan_as_plan(ms) : NULL;
      plans[1] = ss ? mtkx_semi_space_plan_as_plan(ss) : NULL;
      plans[2] = ix ? mtkx_immix_plan_as_plan(ix) : NULL;
      plans[3] = gen ? mtkx_generational_plan_as_plan(gen) : NULL;
    }
    for (i = 0; i < 4; ++i) {
      mtkx_address roots[1] = {0};
      mtkx_address prev = 0;
      int k;
      if (plans[i] == NULL) {
        continue;
      }
      for (k = 0; k < 20; ++k) {
        mtkx_alloc_result a = mtkx_plan_allocate(plans[i], sizeof(node), MTKX_ALIGNOF(node));
        if (!a.ok) {
          break;
        }
        mtkx_from_address(node, a.value)->next = prev;
        prev = a.value;
      }
      roots[0] = prev;
      (void)mtkx_plan_collect(plans[i], roots, prev != 0 ? 1 : 0,
                              mtkx_trace_make(trace_node, NULL));
      mtkx_plan_destroy(plans[i]);
    }
  }
  /* spaces and allocators */
  {
    static double buf[1024];
    const mtkx_address s = mtkx_as_address(buf);
    mtkx_free_list_space fl;
    mtkx_segregated_allocator seg;
    mtkx_immix_space ix;
    mtkx_card_table cards;
    if (mtkx_free_list_space_init(&fl, "fl", s, s + sizeof buf) == MTKX_OK) {
      mtkx_alloc_result a = mtkx_free_list_space_allocate(&fl, 64, 16);
      mtkx_alloc_result b = mtkx_free_list_space_allocate(&fl, 64, 64);
      if (a.ok) (void)mtkx_free_list_space_deallocate(&fl, a.value, 64);
      if (b.ok) (void)mtkx_free_list_space_deallocate(&fl, b.value, 64);
      mtkx_free_list_space_destroy(&fl);
    }
    if (mtkx_segregated_allocator_init(&seg, s, s + sizeof buf) == MTKX_OK) {
      (void)mtkx_segregated_allocator_allocate(&seg, 24, 8);
    }
    mtkx_segregated_allocator_destroy(&seg);
    if (mtkx_immix_space_init(&ix, 100) == MTKX_OK) {
      mtkx_immix_space_destroy(&ix);
    }
    if (mtkx_card_table_init(&cards, s, sizeof buf, 64) == MTKX_OK) {
      mtkx_card_table_destroy(&cards);
    }
  }
  /* scheduler */
  {
    mtkx_coordinator *c = mtkx_coordinator_create(1);
    mtkx_task_pipeline p;
    mtkx_work_queue q;
    if (c != NULL) {
      mtkx_thread_record r;
      mtkx_address_vec roots = MTKX_ADDRESS_VEC_INIT;
      mtkx_thread_record_init(&r, 1, "m");
      (void)mtkx_coordinator_register_mutator(c, &r);
      (void)mtkx_coordinator_collect_roots(c, &roots);
      mtkx_coordinator_destroy(c);
    }
    mtkx_task_pipeline_init(&p);
    (void)mtkx_task_pipeline_append(&p, mtkx_make_phase("a", NULL, NULL, true));
    mtkx_task_pipeline_destroy(&p);
    mtkx_work_queue_init(&q);
    (void)mtkx_work_queue_push(&q, mtkx_work_packet_make(MTKX_WORK_MARK, 1, 0));
    mtkx_work_queue_destroy(&q);
  }
  /* DSL */
  {
    mtkx_parser *p = mtkx_parser_seq(
        mtkx_parser_many(mtkx_parser_satisfy(digit, NULL, "digit")),
        mtkx_parser_alt(mtkx_parser_ch('x'), mtkx_parser_labeled(mtkx_parser_ch('y'), "y")));
    mtkx_ast_node *n = mtkx_ast_node_of("a", 2, mtkx_ast_leaf("b", "1"), mtkx_ast_leaf_int("c", 2));
    char *dump = mtkx_ast_dump(n);
    mtkx_peg_definition *peg = mtkx_peg_definition_create();
    mtkx_task_chain chain;
    mtkx_task_state state;
    mtkx_memo memo;
    if (p != NULL) {
      mtkx_parse_result r = mtkx_run_parser(p, "123z", 4);
      mtkx_parse_result_free(&r);
      r = mtkx_run_parser(p, "12y", 3);
      mtkx_parse_result_free(&r);
      mtkx_parser_release(p);
    }
    mtkx_free(dump);
    mtkx_ast_free(n);
    if (peg != NULL) {
      mtkx_peg_rule *rule = NULL;
      if (mtkx_peg_definition_add_rule(peg, "[a-z]+", NULL, NULL, &rule) == MTKX_OK) {
        mtkx_peg_matcher *m = mtkx_peg_matcher_create(peg, "abc", 3);
        mtkx_parser *rp = mtkx_peg_many1(mtkx_peg_rule_parser(rule));
        (void)mtkx_peg_rule_set_channel(rule, "@X");
        if (m != NULL) {
          (void)mtkx_peg_matcher_feed(m, mtkx_peg_channel_arm("@X", NULL, NULL));
          mtkx_peg_matcher_destroy(m);
        }
        if (rp != NULL) {
          mtkx_parse_result r = mtkx_run_parser(rp, "ab", 2);
          mtkx_parse_result_free(&r);
          mtkx_parser_release(rp);
        }
      }
      mtkx_peg_definition_destroy(peg);
    }
    mtkx_task_chain_init(&chain, MTKX_TASK_STOP_ON_ERROR);
    mtkx_task_state_init(&state);
    (void)mtkx_task_chain_append(&chain, mtkx_task_make("t", NULL, NULL));
    (void)mtkx_task_run(&state, &chain);
    mtkx_task_state_destroy(&state);
    mtkx_task_chain_destroy(&chain);
    if (mtkx_memo_init(&memo, 1, 1, NULL, NULL) == MTKX_OK) {
      mtkx_memo_destroy(&memo);
    }
  }
}

int main(void) {
  mtkx_allocator_hooks hooks;
  long n;
  hooks.malloc_fn = test_malloc;
  hooks.calloc_fn = test_calloc;
  hooks.realloc_fn = test_realloc;
  hooks.free_fn = test_free;
  mtkx_set_allocator(&hooks);

  for (n = 0;; ++n) {
    countdown = n;
    injected = 0;
    workload();
    CHECK(live_blocks == 0); /* every failure path released what it took */
    if (!injected) {
      break;
    }
  }
  CHECK(n > 50); /* the workload really exercised many allocation sites */
  mtkx_set_allocator(NULL);
  printf("fault-injection runs: %ld\n", n + 1);
  return 0;
}
