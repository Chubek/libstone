#include "memtkx/scheduler/coordinator.h"

#include <string.h>

#include "internal/common.h"
#include "internal/platform.h"

typedef struct mtkx__record {
  mtkx_thread_record record; /* record.name points to `name` */
  char *name;
} mtkx__record;

struct mtkx_coordinator {
  mtkx__mutex mutex;
  mtkx__cond cv;
  mtkx__record *records;
  size_t count;
  size_t capacity;
  size_t expected_mutators;
  uint64_t generation;
};

const char *mtkx_mutator_status_string(mtkx_mutator_status status) {
  switch (status) {
    case MTKX_MUTATOR_RUNNING:
      return "running";
    case MTKX_MUTATOR_POLLING:
      return "polling";
    case MTKX_MUTATOR_AT_SAFEPOINT:
      return "at-safepoint";
    case MTKX_MUTATOR_BLOCKED:
      return "blocked";
  }
  return "unknown";
}

mtkx_coordinator *mtkx_coordinator_create(size_t expected_mutators) {
  mtkx_coordinator *c = (mtkx_coordinator *)mtkx_calloc(1, sizeof *c);
  if (c == NULL) {
    return NULL;
  }
  if (mtkx__mutex_init(&c->mutex) != MTKX_OK) {
    mtkx_free(c);
    return NULL;
  }
  if (mtkx__cond_init(&c->cv) != MTKX_OK) {
    mtkx__mutex_destroy(&c->mutex);
    mtkx_free(c);
    return NULL;
  }
  c->expected_mutators = expected_mutators;
  return c;
}

void mtkx_coordinator_destroy(mtkx_coordinator *coordinator) {
  size_t i;
  if (coordinator == NULL) {
    return;
  }
  for (i = 0; i < coordinator->count; ++i) {
    mtkx_free(coordinator->records[i].name);
  }
  mtkx_free(coordinator->records);
  mtkx__cond_destroy(&coordinator->cv);
  mtkx__mutex_destroy(&coordinator->mutex);
  mtkx_free(coordinator);
}

/* Caller holds the mutex. */
static mtkx__record *mtkx__find(mtkx_coordinator *c, mtkx_thread_id id) {
  size_t i;
  for (i = 0; i < c->count; ++i) {
    if (c->records[i].record.id == id) {
      return &c->records[i];
    }
  }
  return NULL;
}

mtkx_status mtkx_coordinator_register_mutator(mtkx_coordinator *coordinator,
                                              const mtkx_thread_record *record) {
  mtkx_status status = MTKX_OK;
  char *name;
  if (record == NULL) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  name = mtkx_strdup(record->name != NULL ? record->name : "");
  if (name == NULL) {
    return MTKX_ERR_NOMEM;
  }
  mtkx__mutex_lock(&coordinator->mutex);
  if (mtkx__find(coordinator, record->id) != NULL) {
    status = MTKX_ERR_DUPLICATE;
  } else if (MTKX__GROW(coordinator->records, coordinator->capacity,
                        coordinator->count + 1) != MTKX_OK) {
    status = MTKX_ERR_NOMEM;
  } else {
    mtkx__record *slot = &coordinator->records[coordinator->count++];
    slot->record = *record;
    slot->name = name;
    slot->record.name = name;
    name = NULL;
  }
  mtkx__mutex_unlock(&coordinator->mutex);
  mtkx_free(name);
  return status;
}

bool mtkx_coordinator_unregister_mutator(mtkx_coordinator *coordinator, mtkx_thread_id id) {
  mtkx__record *found;
  char *name = NULL;
  mtkx__mutex_lock(&coordinator->mutex);
  found = mtkx__find(coordinator, id);
  if (found != NULL) {
    size_t index = (size_t)(found - coordinator->records);
    name = found->name;
    coordinator->records[index] = coordinator->records[coordinator->count - 1];
    --coordinator->count;
  }
  mtkx__mutex_unlock(&coordinator->mutex);
  mtkx_free(name);
  return found != NULL;
}

void mtkx_coordinator_update_status(mtkx_coordinator *coordinator, mtkx_thread_id id,
                                    mtkx_mutator_status status) {
  mtkx__record *found;
  mtkx__mutex_lock(&coordinator->mutex);
  found = mtkx__find(coordinator, id);
  if (found != NULL) {
    found->record.status = status;
  }
  mtkx__cond_broadcast(&coordinator->cv);
  mtkx__mutex_unlock(&coordinator->mutex);
}

mtkx_mutator_status mtkx_coordinator_status(mtkx_coordinator *coordinator,
                                            mtkx_thread_id id) {
  mtkx__record *found;
  mtkx_mutator_status status;
  mtkx__mutex_lock(&coordinator->mutex);
  found = mtkx__find(coordinator, id);
  status = found != NULL ? found->record.status : MTKX_MUTATOR_BLOCKED;
  mtkx__mutex_unlock(&coordinator->mutex);
  return status;
}

/* Caller holds the mutex. */
static bool mtkx__all_at_safepoint(const mtkx_coordinator *c) {
  size_t i;
  if (c->count < c->expected_mutators) {
    return false;
  }
  for (i = 0; i < c->count; ++i) {
    if (c->records[i].record.status != MTKX_MUTATOR_AT_SAFEPOINT) {
      return false;
    }
  }
  return true;
}

mtkx_gc_result mtkx_coordinator_request_stop_the_world(mtkx_coordinator *coordinator,
                                                       uint64_t timeout_ms) {
  uint64_t deadline = mtkx__deadline_after_ms(timeout_ms);
  bool ready;
  int generation;
  mtkx__mutex_lock(&coordinator->mutex);
  ++coordinator->generation;
  for (;;) {
    ready = mtkx__all_at_safepoint(coordinator);
    if (ready) {
      break;
    }
    if (!mtkx__cond_wait_until(&coordinator->cv, &coordinator->mutex, deadline)) {
      ready = mtkx__all_at_safepoint(coordinator);
      break;
    }
  }
  generation = (int)coordinator->generation;
  mtkx__mutex_unlock(&coordinator->mutex);
  if (!ready) {
    return mtkx_gc_result_from_err(MTKX_GC_SAFEPOINT_TIMEOUT);
  }
  return mtkx_gc_result_from_ok(generation);
}

void mtkx_coordinator_release_mutators(mtkx_coordinator *coordinator) {
  size_t i;
  mtkx__mutex_lock(&coordinator->mutex);
  ++coordinator->generation;
  for (i = 0; i < coordinator->count; ++i) {
    coordinator->records[i].record.status = MTKX_MUTATOR_RUNNING;
  }
  mtkx__cond_broadcast(&coordinator->cv);
  mtkx__mutex_unlock(&coordinator->mutex);
}

mtkx_gc_result mtkx_coordinator_poll_safepoint(mtkx_coordinator *coordinator,
                                               mtkx_thread_id id) {
  mtkx__record *found;
  uint64_t generation;
  uint64_t deadline;
  bool released;
  int current;

  mtkx__mutex_lock(&coordinator->mutex);
  found = mtkx__find(coordinator, id);
  if (found == NULL) {
    mtkx__mutex_unlock(&coordinator->mutex);
    return mtkx_gc_result_from_err(MTKX_GC_MUTATOR_NOT_REGISTERED);
  }
  found->record.status = MTKX_MUTATOR_AT_SAFEPOINT;
  generation = coordinator->generation;
  mtkx__cond_broadcast(&coordinator->cv);

  deadline = mtkx__deadline_after_ms(MTKX_SAFEPOINT_POLL_TIMEOUT_MS);
  for (;;) {
    released = coordinator->generation != generation;
    if (released) {
      break;
    }
    if (!mtkx__cond_wait_until(&coordinator->cv, &coordinator->mutex, deadline)) {
      released = coordinator->generation != generation;
      break;
    }
  }
  current = (int)coordinator->generation;
  mtkx__mutex_unlock(&coordinator->mutex);
  if (!released) {
    return mtkx_gc_result_from_err(MTKX_GC_SAFEPOINT_TIMEOUT);
  }
  return mtkx_gc_result_from_ok(current);
}

mtkx_status mtkx_coordinator_collect_roots(mtkx_coordinator *coordinator,
                                           mtkx_address_vec *roots) {
  size_t i;
  mtkx_status status = MTKX_OK;
  mtkx__mutex_lock(&coordinator->mutex);
  for (i = 0; i < coordinator->count && status == MTKX_OK; ++i) {
    const mtkx_thread_record *r = &coordinator->records[i].record;
    if (r->stack_scan != NULL) {
      status = r->stack_scan(r->id, roots, r->stack_scan_ctx);
    }
    if (status == MTKX_OK && r->root_provider != NULL) {
      status = r->root_provider(roots, r->root_provider_ctx);
    }
  }
  mtkx__mutex_unlock(&coordinator->mutex);
  return status;
}

size_t mtkx_coordinator_registered_mutators(mtkx_coordinator *coordinator) {
  size_t n;
  mtkx__mutex_lock(&coordinator->mutex);
  n = coordinator->count;
  mtkx__mutex_unlock(&coordinator->mutex);
  return n;
}

size_t mtkx_coordinator_expected_mutators(const mtkx_coordinator *coordinator) {
  return coordinator->expected_mutators;
}

mtkx_gc_result mtkx_mutator_context_poll(const mtkx_mutator_context *context) {
  return mtkx_coordinator_poll_safepoint(context->coordinator, context->id);
}

mtkx_gc_result mtkx_collector_context_stop_the_world(const mtkx_collector_context *context,
                                                     uint64_t timeout_ms) {
  return mtkx_coordinator_request_stop_the_world(context->coordinator, timeout_ms);
}

void mtkx_collector_context_release_mutators(const mtkx_collector_context *context) {
  mtkx_coordinator_release_mutators(context->coordinator);
}

mtkx_status mtkx_collector_context_roots(const mtkx_collector_context *context,
                                         mtkx_address_vec *roots) {
  return mtkx_coordinator_collect_roots(context->coordinator, roots);
}
