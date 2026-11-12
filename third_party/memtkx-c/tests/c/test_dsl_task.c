/* The DSL task chain: run, policies, suspend and upon. */
#include <memtkx/memtkx.h>

#include "test_support.h"

static mtkx_task_result ok_task(mtkx_task_state *state, void *ctx) {
  (void)state;
  return mtkx_task_ok((const char *)ctx);
}
static mtkx_task_result failing_task(mtkx_task_state *state, void *ctx) {
  (void)state;
  return mtkx_task_err((const char *)ctx);
}
static mtkx_task_result suspending_task(mtkx_task_state *state, void *ctx) {
  (void)ctx;
  mtkx_task_suspend(state);
  return mtkx_task_ok("suspended");
}
static mtkx_task_result reads_previous(mtkx_task_state *state, void *ctx) {
  const mtkx_task_result *prev = mtkx_task_state_find(state, (const char *)ctx);
  return prev != NULL && prev->ok ? mtkx_task_ok("saw it") : mtkx_task_err("missing");
}
static bool yes(void *ctx) {
  (void)ctx;
  return true;
}
static bool no(void *ctx) {
  (void)ctx;
  return false;
}

int main(void) {
  mtkx_task_chain chain;
  mtkx_task_state state;

  /* stop on error */
  mtkx_task_chain_init(&chain, MTKX_TASK_STOP_ON_ERROR);
  CHECK(mtkx_task_chain_append(&chain, mtkx_task_make("load", ok_task, (void *)"ok")) ==
        MTKX_OK);
  CHECK(mtkx_task_chain_append(&chain, mtkx_task_make("check", reads_previous,
                                                      (void *)"load")) == MTKX_OK);
  CHECK(mtkx_task_chain_append(&chain, mtkx_task_make("fail", failing_task,
                                                      (void *)"broken")) == MTKX_OK);
  CHECK(mtkx_task_chain_append(&chain, mtkx_task_make("after", ok_task, (void *)"x")) ==
        MTKX_OK);
  mtkx_task_state_init(&state);
  CHECK(mtkx_task_run(&state, &chain) == MTKX_OK);
  CHECK(state.count == 3);
  CHECK_STR_EQ(mtkx_task_state_find(&state, "check")->text, "saw it");
  CHECK(!mtkx_task_state_find(&state, "fail")->ok);
  CHECK_STR_EQ(mtkx_task_state_find(&state, "fail")->text, "broken");
  CHECK(mtkx_task_state_find(&state, "after") == NULL);

  /* continue on error, insert-or-assign on rerun */
  chain.policy = MTKX_TASK_CONTINUE_ON_ERROR;
  CHECK(mtkx_task_run(&state, &chain) == MTKX_OK);
  CHECK(state.count == 4);
  CHECK_STR_EQ(mtkx_task_state_find(&state, "after")->text, "x");
  mtkx_task_state_destroy(&state);
  mtkx_task_chain_destroy(&chain);

  /* suspend stops the chain after the suspending task */
  mtkx_task_chain_init(&chain, MTKX_TASK_STOP_ON_ERROR);
  CHECK(mtkx_task_chain_append(&chain, mtkx_task_make("pause", suspending_task, NULL)) ==
        MTKX_OK);
  CHECK(mtkx_task_chain_append(&chain, mtkx_task_make("never", ok_task, (void *)"n")) ==
        MTKX_OK);
  mtkx_task_state_init(&state);
  CHECK(mtkx_task_run(&state, &chain) == MTKX_OK);
  CHECK(state.suspended && state.count == 1);

  /* upon runs a task only when its event fires */
  CHECK(mtkx_task_upon(&state, no, NULL, mtkx_task_make("event", ok_task, (void *)"e")) ==
        MTKX_OK);
  CHECK(mtkx_task_state_find(&state, "event") == NULL);
  CHECK(mtkx_task_upon(&state, yes, NULL, mtkx_task_make("event", ok_task, (void *)"e")) ==
        MTKX_OK);
  CHECK_STR_EQ(mtkx_task_state_find(&state, "event")->text, "e");
  {
    mtkx_task noop = mtkx_task_make("noop", NULL, NULL);
    mtkx_task_result r = mtkx_task_invoke(&noop, &state);
    CHECK(r.ok && strcmp(r.text, "") == 0);
    mtkx_task_result_free(&r);
  }
  mtkx_task_state_destroy(&state);
  mtkx_task_chain_destroy(&chain);
  return 0;
}
