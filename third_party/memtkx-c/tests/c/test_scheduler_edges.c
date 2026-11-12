/* Port of tests/test_scheduler_edges.cpp. */
#include <memtkx/memtkx.h>

#include "test_support.h"

static mtkx_gc_error observe(mtkx_gc_error error, void *ctx) {
  CHECK(error == MTKX_GC_SAFEPOINT_TIMEOUT);
  *(int *)ctx += 1;
  return error;
}

static mtkx_gc_result warning_phase(mtkx_phase_state *state, void *ctx) {
  (void)state;
  (void)ctx;
  return mtkx_gc_result_from_err(MTKX_GC_INVALID_TRACE);
}

static mtkx_gc_result continue_phase(mtkx_phase_state *state, void *ctx) {
  (void)ctx;
  mtkx_phase_state_note_completion(state, 1);
  return mtkx_gc_result_from_ok(1);
}

int main(void) {
  mtkx_coordinator *coordinator = mtkx_coordinator_create(2);
  mtkx_gc_result timed_out;
  mtkx_thread_record record;
  int observed = 0;
  mtkx_task_pipeline pipeline;
  mtkx_phase_state state;
  mtkx_gc_result result;

  CHECK(coordinator != NULL);
  timed_out = mtkx_coordinator_request_stop_the_world(coordinator, 10);
  CHECK(mtkx_gc_result_is_err(timed_out));
  (void)mtkx_gc_result_map_err(timed_out, observe, &observed);
  CHECK(observed == 1);

  mtkx_thread_record_init(&record, 1, "mutator");
  CHECK(mtkx_coordinator_register_mutator(coordinator, &record) == MTKX_OK);
  CHECK(mtkx_coordinator_register_mutator(coordinator, &record) == MTKX_ERR_DUPLICATE);
  CHECK(mtkx_coordinator_registered_mutators(coordinator) == 1);
  CHECK(mtkx_coordinator_status(coordinator, 1) == MTKX_MUTATOR_RUNNING);
  CHECK(mtkx_coordinator_status(coordinator, 99) == MTKX_MUTATOR_BLOCKED);
  CHECK(mtkx_coordinator_poll_safepoint(coordinator, 99).error ==
        MTKX_GC_MUTATOR_NOT_REGISTERED);
  CHECK(mtkx_coordinator_unregister_mutator(coordinator, 1));
  CHECK(!mtkx_coordinator_unregister_mutator(coordinator, 1));
  mtkx_coordinator_destroy(coordinator);

  mtkx_task_pipeline_init(&pipeline);
  CHECK(mtkx_task_pipeline_append(&pipeline, mtkx_make_phase("warning", warning_phase, NULL,
                                                             /*stop_on_error=*/false)) ==
        MTKX_OK);
  CHECK(mtkx_task_pipeline_append(&pipeline,
                                  mtkx_make_phase("continue", continue_phase, NULL, true)) ==
        MTKX_OK);
  mtkx_phase_state_init(&state);
  result = mtkx_task_pipeline_run(&pipeline, &state);
  CHECK(mtkx_gc_result_is_ok(result));
  CHECK(state.completed_steps == 1);

  /* a stop_on_error failure ends the run with its error */
  pipeline.phases[0].stop_on_error = true;
  result = mtkx_task_pipeline_run(&pipeline, &state);
  CHECK(result.error == MTKX_GC_INVALID_TRACE);
  CHECK_STR_EQ(state.phase_name, "warning");
  mtkx_task_pipeline_destroy(&pipeline);

  CHECK_STR_EQ(mtkx_gc_error_string(MTKX_GC_SAFEPOINT_TIMEOUT), "safepoint timed out");
  CHECK_STR_EQ(mtkx_mutator_status_string(MTKX_MUTATOR_AT_SAFEPOINT), "at-safepoint");
  return 0;
}
