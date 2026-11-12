/**
 * @file memtkx/scheduler/coordinator.h
 * @brief Cooperative mutator registration, safepoint coordination and root
 *        collection (C++ `CollectorCoordinator`, `MutatorContext`,
 *        `CollectorContext`).
 *
 * The coordinator is thread-safe (mutex + condition variable on POSIX
 * threads or Win32). Status is cooperative metadata: updating it does not
 * pause a thread. See the guide for a complete parking protocol.
 */
#ifndef MEMTKX_SCHEDULER_COORDINATOR_H
#define MEMTKX_SCHEDULER_COORDINATOR_H

#include <stddef.h>
#include <stdint.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"
#include "memtkx/core/types.h"
#include "memtkx/core/vec.h"

MTKX_BEGIN_DECLS

/** Application-assigned mutator identifier. */
typedef uint64_t mtkx_thread_id;

/** Default stop-the-world timeout (2 s). */
#define MTKX_DEFAULT_STOP_THE_WORLD_TIMEOUT_MS 2000u
/** How long a safepoint poll waits for release (5 s). */
#define MTKX_SAFEPOINT_POLL_TIMEOUT_MS 5000u

/** Cooperative mutator states. */
typedef enum mtkx_mutator_status {
  MTKX_MUTATOR_RUNNING = 0,
  MTKX_MUTATOR_POLLING = 1,
  MTKX_MUTATOR_AT_SAFEPOINT = 2,
  MTKX_MUTATOR_BLOCKED = 3
} mtkx_mutator_status;

/** @return "running", "polling", "at-safepoint" or "blocked". */
MTKX_API const char *mtkx_mutator_status_string(mtkx_mutator_status status);

/**
 * @brief Appends global roots to @p roots (C++ `RootProvider`).
 *        Runs under the coordinator mutex: must not call back into it.
 */
typedef mtkx_status (*mtkx_root_provider_fn)(mtkx_address_vec *roots, void *ctx);

/**
 * @brief Appends the stack roots of thread @p id (C++ `StackScanCallback`).
 *        Runs under the coordinator mutex: must not call back into it.
 */
typedef mtkx_status (*mtkx_stack_scan_fn)(mtkx_thread_id id, mtkx_address_vec *roots,
                                          void *ctx);

/** Registration data (C++ `ThreadRecord`). Registration copies the name. */
typedef struct mtkx_thread_record {
  mtkx_thread_id id;
  const char *name;
  mtkx_mutator_status status;
  mtkx_stack_scan_fn stack_scan;
  void *stack_scan_ctx;
  mtkx_root_provider_fn root_provider;
  void *root_provider_ctx;
} mtkx_thread_record;

/** Initialises a record with @p id and @p name, status Running, no callbacks. */
MTKX_INLINE void mtkx_thread_record_init(mtkx_thread_record *record, mtkx_thread_id id,
                                         const char *name) {
  record->id = id;
  record->name = name;
  record->status = MTKX_MUTATOR_RUNNING;
  record->stack_scan = NULL;
  record->stack_scan_ctx = NULL;
  record->root_provider = NULL;
  record->root_provider_ctx = NULL;
}

/** The coordinator (opaque). */
typedef struct mtkx_coordinator mtkx_coordinator;

/**
 * @brief Creates a coordinator. Stop-the-world requires at least
 *        @p expected_mutators registered records (C++ default: 1).
 * @return NULL on allocation or OS failure.
 */
MTKX_API mtkx_coordinator *mtkx_coordinator_create(size_t expected_mutators);

/** Destroys the coordinator. No thread may be waiting in it. */
MTKX_API void mtkx_coordinator_destroy(mtkx_coordinator *coordinator);

/**
 * @brief Registers a mutator.
 * @return MTKX_OK, MTKX_ERR_DUPLICATE (id already registered) or MTKX_ERR_NOMEM.
 */
MTKX_API mtkx_status mtkx_coordinator_register_mutator(mtkx_coordinator *coordinator,
                                                       const mtkx_thread_record *record);

/** @return true when a record was removed. */
MTKX_API bool mtkx_coordinator_unregister_mutator(mtkx_coordinator *coordinator,
                                                  mtkx_thread_id id);

/** Sets a record's status and wakes waiters. */
MTKX_API void mtkx_coordinator_update_status(mtkx_coordinator *coordinator,
                                             mtkx_thread_id id, mtkx_mutator_status status);

/** @return the record's status, or MTKX_MUTATOR_BLOCKED for unknown ids. */
MTKX_API mtkx_mutator_status mtkx_coordinator_status(mtkx_coordinator *coordinator,
                                                     mtkx_thread_id id);

/**
 * @brief Advances the generation, then waits until at least the expected
 *        number of records are registered and every record is
 *        AtSafepoint.
 * @return ok(generation) or MTKX_GC_SAFEPOINT_TIMEOUT.
 */
MTKX_API mtkx_gc_result mtkx_coordinator_request_stop_the_world(mtkx_coordinator *coordinator,
                                                                uint64_t timeout_ms);

/** Advances the generation, sets every record Running and wakes waiters. */
MTKX_API void mtkx_coordinator_release_mutators(mtkx_coordinator *coordinator);

/**
 * @brief Marks @p id AtSafepoint and waits (up to
 *        MTKX_SAFEPOINT_POLL_TIMEOUT_MS) for the generation to change.
 * @return ok(generation), MTKX_GC_MUTATOR_NOT_REGISTERED or
 *         MTKX_GC_SAFEPOINT_TIMEOUT.
 */
MTKX_API mtkx_gc_result mtkx_coordinator_poll_safepoint(mtkx_coordinator *coordinator,
                                                        mtkx_thread_id id);

/**
 * @brief Appends every record's stack and global roots to @p roots,
 *        calling the callbacks under the coordinator mutex.
 * @return MTKX_OK or the first non-OK status returned by a callback.
 */
MTKX_API mtkx_status mtkx_coordinator_collect_roots(mtkx_coordinator *coordinator,
                                                    mtkx_address_vec *roots);

/** @return the number of registered records. */
MTKX_API size_t mtkx_coordinator_registered_mutators(mtkx_coordinator *coordinator);

/** @return the expected mutator count given at creation. */
MTKX_API size_t mtkx_coordinator_expected_mutators(const mtkx_coordinator *coordinator);

/* ------------------------------------------------------------------ */
/*  Contexts                                                          */
/* ------------------------------------------------------------------ */

/** A mutator's handle (C++ `MutatorContext`). */
typedef struct mtkx_mutator_context {
  mtkx_coordinator *coordinator;
  mtkx_thread_id id;
} mtkx_mutator_context;

/** Builds a mutator context. */
MTKX_INLINE mtkx_mutator_context mtkx_mutator_context_make(mtkx_coordinator *coordinator,
                                                           mtkx_thread_id id) {
  mtkx_mutator_context c;
  c.coordinator = coordinator;
  c.id = id;
  return c;
}

/** Polls the safepoint for this mutator. */
MTKX_API mtkx_gc_result mtkx_mutator_context_poll(const mtkx_mutator_context *context);

/** The collector's handle (C++ `CollectorContext`). */
typedef struct mtkx_collector_context {
  mtkx_coordinator *coordinator;
} mtkx_collector_context;

/** Builds a collector context. */
MTKX_INLINE mtkx_collector_context mtkx_collector_context_make(mtkx_coordinator *coordinator) {
  mtkx_collector_context c;
  c.coordinator = coordinator;
  return c;
}

/** Requests stop-the-world (pass MTKX_DEFAULT_STOP_THE_WORLD_TIMEOUT_MS for the default). */
MTKX_API mtkx_gc_result mtkx_collector_context_stop_the_world(
    const mtkx_collector_context *context, uint64_t timeout_ms);

/** Releases mutators. */
MTKX_API void mtkx_collector_context_release_mutators(const mtkx_collector_context *context);

/** Collects roots into @p roots. */
MTKX_API mtkx_status mtkx_collector_context_roots(const mtkx_collector_context *context,
                                                  mtkx_address_vec *roots);

MTKX_END_DECLS

#endif /* MEMTKX_SCHEDULER_COORDINATOR_H */
