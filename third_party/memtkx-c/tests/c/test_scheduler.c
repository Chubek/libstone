/* Port of tests/test_scheduler.cpp plus the guide's park and pipeline examples. */
#include <memtkx/memtkx.h>

#include "test_support.h"
#include "test_thread.h"

static void check_packet(const mtkx_work_packet *packet, void *ctx) {
  (void)ctx;
  CHECK(packet->object != 0);
}

static void requeue_once(const mtkx_work_packet *packet, void *ctx) {
  mtkx_work_queue *queue = (mtkx_work_queue *)ctx;
  if (packet->kind == MTKX_WORK_ROOT_SCAN) {
    CHECK(mtkx_work_queue_push(queue, mtkx_work_packet_make(MTKX_WORK_MARK, 30, 0)) ==
          MTKX_OK);
  }
}

static mtkx_gc_result stop(mtkx_phase_state *s, void *c) {
  (void)s;
  (void)c;
  return mtkx_gc_result_from_ok(1);
}
static mtkx_gc_result roots(mtkx_phase_state *s, void *c) {
  (void)c;
  mtkx_phase_state_note_completion(s, 2);
  return mtkx_gc_result_from_ok(2);
}
static mtkx_gc_result closure(mtkx_phase_state *s, void *c) {
  (void)s;
  (void)c;
  return mtkx_gc_result_from_ok(3);
}

typedef struct mutator_args {
  mtkx_mutator_context mutator;
  int ok;
} mutator_args;

static void mutator_main(void *p) {
  mutator_args *args = (mutator_args *)p;
  args->ok = mtkx_gc_result_is_ok(mtkx_mutator_context_poll(&args->mutator));
}

/* guide 11.4: application-managed park with a root provider */
static mtkx_status provide_roots(mtkx_address_vec *out, void *ctx) {
  return mtkx_address_vec_push(out, *(const mtkx_address *)ctx);
}

/* The handshake uses the coordinator's own (mutex-protected) status:
 * the mutator publishes AtSafepoint and stays parked until the collector's
 * release sets it back to Running. */
static void park_main(void *p) {
  mtkx_coordinator *coordinator = (mtkx_coordinator *)p;
  mtkx_coordinator_update_status(coordinator, 1, MTKX_MUTATOR_AT_SAFEPOINT);
  while (mtkx_coordinator_status(coordinator, 1) == MTKX_MUTATOR_AT_SAFEPOINT) {
    test_sleep_ms(1);
  }
}

int main(void) {
  /* --- work queue --- */
  {
    mtkx_work_queue queue;
    size_t processed;
    size_t i;
    mtkx_work_packet packet;
    mtkx_work_queue_init(&queue);
    CHECK(mtkx_work_queue_push(&queue, mtkx_work_packet_make(MTKX_WORK_ROOT_SCAN, 10, 0)) ==
          MTKX_OK);
    CHECK(mtkx_work_queue_push(&queue, mtkx_work_packet_make(MTKX_WORK_MARK, 20, 0)) ==
          MTKX_OK);
    processed = mtkx_work_queue_drain(&queue, check_packet, NULL);
    CHECK(processed == 2);
    CHECK(mtkx_work_queue_empty(&queue));

    /* draining includes packets pushed by the handler */
    CHECK(mtkx_work_queue_push(&queue, mtkx_work_packet_make(MTKX_WORK_ROOT_SCAN, 10, 0)) ==
          MTKX_OK);
    CHECK(mtkx_work_queue_drain(&queue, requeue_once, &queue) == 2);

    /* FIFO order across ring-buffer growth and wrap-around */
    for (i = 0; i < 10; ++i) {
      CHECK(mtkx_work_queue_push(&queue, mtkx_work_packet_make(MTKX_WORK_USER, i + 1, i)) ==
            MTKX_OK);
    }
    for (i = 0; i < 5; ++i) {
      CHECK(mtkx_work_queue_try_pop(&queue, &packet) && packet.extra == i);
    }
    for (i = 10; i < 40; ++i) {
      CHECK(mtkx_work_queue_push(&queue, mtkx_work_packet_make(MTKX_WORK_USER, i + 1, i)) ==
            MTKX_OK);
    }
    for (i = 5; i < 40; ++i) {
      CHECK(mtkx_work_queue_try_pop(&queue, &packet) && packet.extra == i);
    }
    CHECK(!mtkx_work_queue_try_pop(&queue, &packet));
    mtkx_work_queue_destroy(&queue);
  }

  /* --- phase pipeline --- */
  {
    mtkx_task_pipeline pipeline, tail, combined;
    mtkx_phase_state state;
    mtkx_gc_result result;
    mtkx_task_pipeline_init(&pipeline);
    mtkx_task_pipeline_init(&tail);
    mtkx_task_pipeline_init(&combined);
    CHECK(mtkx_task_pipeline_append(&pipeline, mtkx_make_phase("stop", stop, NULL, true)) ==
          MTKX_OK);
    CHECK(mtkx_task_pipeline_append(&pipeline, mtkx_make_phase("roots", roots, NULL, true)) ==
          MTKX_OK);
    CHECK(mtkx_task_pipeline_append(&tail, mtkx_make_phase("closure", closure, NULL, true)) ==
          MTKX_OK);
    CHECK(mtkx_task_pipeline_append_pipeline(&pipeline, &tail) == MTKX_OK);
    mtkx_phase_state_init(&state);
    result = mtkx_task_pipeline_run(&pipeline, &state);
    CHECK(mtkx_gc_result_is_ok(result));
    CHECK(mtkx_gc_result_unwrap(result) == 2);
    CHECK(state.completed_steps == 2);
    CHECK(mtkx_task_pipeline_size(&pipeline) == 3);
    CHECK_STR_EQ(state.phase_name, "closure");
    CHECK(mtkx_task_pipeline_append_pipeline(&combined, &pipeline) == MTKX_OK);
    CHECK(mtkx_task_pipeline_append_pipeline(&combined, &combined) == MTKX_OK);
    CHECK(mtkx_task_pipeline_size(&combined) == 6);
    CHECK(mtkx_task_pipeline_append(&combined, mtkx_make_phase("noop", NULL, NULL, true)) ==
          MTKX_OK);
    CHECK(mtkx_gc_result_unwrap(mtkx_task_pipeline_run(&combined, &state)) == 4);
    mtkx_task_pipeline_destroy(&pipeline);
    mtkx_task_pipeline_destroy(&tail);
    mtkx_task_pipeline_destroy(&combined);
    CHECK(mtkx_task_pipeline_empty(&pipeline));
  }

  /* --- coordinator: poll / stop / release --- */
  {
    mtkx_coordinator *coordinator = mtkx_coordinator_create(1);
    mtkx_thread_record record;
    mtkx_collector_context collector;
    mutator_args args;
    test_thread thread;
    mtkx_gc_result stopped;

    CHECK(coordinator != NULL);
    mtkx_thread_record_init(&record, 1, "mutator");
    CHECK(mtkx_coordinator_register_mutator(coordinator, &record) == MTKX_OK);
    CHECK(mtkx_coordinator_registered_mutators(coordinator) == 1);

    args.mutator = mtkx_mutator_context_make(coordinator, 1);
    args.ok = 0;
    collector = mtkx_collector_context_make(coordinator);

    CHECK(test_thread_create(&thread, mutator_main, &args) == 0);
    stopped = mtkx_collector_context_stop_the_world(&collector, 3000);
    CHECK(mtkx_gc_result_is_ok(stopped));
    mtkx_collector_context_release_mutators(&collector);
    test_thread_join(thread);
    CHECK(args.ok);

    CHECK(mtkx_coordinator_unregister_mutator(coordinator, 1));
    CHECK(mtkx_coordinator_registered_mutators(coordinator) == 0);
    mtkx_coordinator_destroy(coordinator);
  }

  /* --- coordinator: application-managed park and root collection --- */
  {
    mtkx_coordinator *coordinator = mtkx_coordinator_create(1);
    mtkx_thread_record record;
    mtkx_address root = 123;
    test_thread thread;
    mtkx_collector_context collector;
    mtkx_address_vec gathered = MTKX_ADDRESS_VEC_INIT;

    CHECK(coordinator != NULL);
    mtkx_thread_record_init(&record, 1, "application-mutator");
    record.root_provider = provide_roots;
    record.root_provider_ctx = &root;
    CHECK(mtkx_coordinator_register_mutator(coordinator, &record) == MTKX_OK);

    CHECK(test_thread_create(&thread, park_main, coordinator) == 0);
    while (mtkx_coordinator_status(coordinator, 1) != MTKX_MUTATOR_AT_SAFEPOINT) {
      test_sleep_ms(1);
    }
    collector = mtkx_collector_context_make(coordinator);
    CHECK(mtkx_gc_result_is_ok(
        mtkx_collector_context_stop_the_world(&collector, MTKX_DEFAULT_STOP_THE_WORLD_TIMEOUT_MS)));
    CHECK(mtkx_collector_context_roots(&collector, &gathered) == MTKX_OK);
    CHECK(gathered.size == 1 && gathered.data[0] == 123);
    mtkx_collector_context_release_mutators(&collector);
    test_thread_join(thread); /* the release lets the mutator resume */
    CHECK(mtkx_coordinator_status(coordinator, 1) == MTKX_MUTATOR_RUNNING);
    CHECK(mtkx_coordinator_unregister_mutator(coordinator, 1));
    mtkx_address_vec_destroy(&gathered);
    mtkx_coordinator_destroy(coordinator);
  }
  return 0;
}
