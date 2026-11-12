/* GUIDE 11.4: an application-managed park (POSIX threads). */
#include <memtkx/memtkx.h>

#include <assert.h>
#include <pthread.h>
#include <sched.h>

static mtkx_status provide_roots(mtkx_address_vec *roots, void *ctx) {
  (void)ctx;
  return mtkx_address_vec_push(roots, 123);
}

static void *mutator(void *arg) {
  mtkx_coordinator *c = (mtkx_coordinator *)arg;
  /* managed writes finish before publishing the safepoint */
  mtkx_coordinator_update_status(c, 1, MTKX_MUTATOR_AT_SAFEPOINT);
  while (mtkx_coordinator_status(c, 1) == MTKX_MUTATOR_AT_SAFEPOINT) {
    sched_yield(); /* parked: no managed memory is touched here */
  }
  return NULL;
}

int main(void) {
  mtkx_coordinator *c = mtkx_coordinator_create(1);
  mtkx_thread_record record;
  pthread_t thread;
  mtkx_collector_context collector;
  mtkx_address_vec roots = MTKX_ADDRESS_VEC_INIT;
  mtkx_gc_result stopped;
  mtkx_status status;
  bool removed;

  if (c == NULL) {
    return 1;
  }
  mtkx_thread_record_init(&record, 1, "application-mutator");
  record.root_provider = provide_roots;
  if (mtkx_coordinator_register_mutator(c, &record) != MTKX_OK) {
    return 1;
  }
  pthread_create(&thread, NULL, mutator, c);

  collector = mtkx_collector_context_make(c);
  stopped = mtkx_collector_context_stop_the_world(&collector,
                                                  MTKX_DEFAULT_STOP_THE_WORLD_TIMEOUT_MS);
  assert(stopped.ok);
  status = mtkx_collector_context_roots(&collector, &roots);
  assert(status == MTKX_OK && roots.size == 1 && roots.data[0] == 123);
  /* collect with the gathered roots here, while the mutator is parked */

  mtkx_collector_context_release_mutators(&collector);
  pthread_join(thread, NULL);
  removed = mtkx_coordinator_unregister_mutator(c, 1);
  assert(removed);
  mtkx_address_vec_destroy(&roots);
  mtkx_coordinator_destroy(c);
  return 0;
}
