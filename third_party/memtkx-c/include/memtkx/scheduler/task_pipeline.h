/**
 * @file memtkx/scheduler/task_pipeline.h
 * @brief Named collection phases run in sequence (C++ `Phase`,
 *        `TaskPipeline`, `PhaseState`, `make_phase`, `phase | phase`).
 *
 * @code
 *   static mtkx_gc_result roots(mtkx_phase_state *st, void *ctx) {
 *     mtkx_phase_state_note_completion(st, 2);
 *     return mtkx_gc_result_from_ok(2);
 *   }
 *
 *   mtkx_task_pipeline pipeline;
 *   mtkx_phase_state state;
 *   mtkx_task_pipeline_init(&pipeline);
 *   mtkx_task_pipeline_append(&pipeline, mtkx_make_phase("roots", roots, NULL, true));
 *   mtkx_phase_state_init(&state);
 *   mtkx_gc_result r = mtkx_task_pipeline_run(&pipeline, &state);  // ok, 2
 *   mtkx_task_pipeline_destroy(&pipeline);
 * @endcode
 */
#ifndef MEMTKX_SCHEDULER_TASK_PIPELINE_H
#define MEMTKX_SCHEDULER_TASK_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

#include "memtkx/core/base.h"
#include "memtkx/core/error.h"

MTKX_BEGIN_DECLS

/** State shared by the phases of one run (C++ `PhaseState`). */
typedef struct mtkx_phase_state {
  /** Name of the running (or last run) phase; points into the pipeline,
   *  valid while that pipeline is alive. NULL before the first run. */
  const char *phase_name;
  /** Incremented by phases via mtkx_phase_state_note_completion(). */
  size_t completed_steps;
  /** Duration of the most recently executed phase, in nanoseconds. */
  uint64_t elapsed_ns;
} mtkx_phase_state;

/** Initialises an empty state. */
MTKX_INLINE void mtkx_phase_state_init(mtkx_phase_state *state) {
  state->phase_name = NULL;
  state->completed_steps = 0;
  state->elapsed_ns = 0;
}

/** Adds @p steps to the completed-step counter. */
MTKX_INLINE void mtkx_phase_state_note_completion(mtkx_phase_state *state, size_t steps) {
  state->completed_steps += steps;
}

/** A phase body. */
typedef mtkx_gc_result (*mtkx_phase_fn)(mtkx_phase_state *state, void *ctx);

/** A phase description. The name is borrowed until appended to a pipeline. */
typedef struct mtkx_phase {
  const char *name;
  mtkx_phase_fn fn;
  void *ctx;
  bool stop_on_error;
} mtkx_phase;

/** Builds a phase (C++ `make_phase(name, fn, stop_on_error = true)`). */
MTKX_INLINE mtkx_phase mtkx_make_phase(const char *name, mtkx_phase_fn fn, void *ctx,
                                       bool stop_on_error) {
  mtkx_phase p;
  p.name = name;
  p.fn = fn;
  p.ctx = ctx;
  p.stop_on_error = stop_on_error;
  return p;
}

/** Runs one phase; a NULL fn succeeds with 0. */
MTKX_API mtkx_gc_result mtkx_phase_run(const mtkx_phase *phase, mtkx_phase_state *state);

/** An ordered list of phases (C++ `TaskPipeline`). Owns copies of the names. */
typedef struct mtkx_task_pipeline {
  mtkx_phase *phases;
  size_t count;
  size_t capacity;
} mtkx_task_pipeline;

/** Initialises an empty pipeline. */
MTKX_API void mtkx_task_pipeline_init(mtkx_task_pipeline *pipeline);
/** Releases the pipeline. */
MTKX_API void mtkx_task_pipeline_destroy(mtkx_task_pipeline *pipeline);
/** Appends a phase (C++ `pipeline | phase`). */
MTKX_API mtkx_status mtkx_task_pipeline_append(mtkx_task_pipeline *pipeline, mtkx_phase phase);
/** Appends copies of every phase of @p other (C++ `pipeline | pipeline`). */
MTKX_API mtkx_status mtkx_task_pipeline_append_pipeline(mtkx_task_pipeline *pipeline,
                                                        const mtkx_task_pipeline *other);

/**
 * @brief Runs the phases in order. `completed_steps` is reset first. A
 *        failing phase with stop_on_error ends the run with its error.
 * @return ok(completed_steps) after the last phase.
 */
MTKX_API mtkx_gc_result mtkx_task_pipeline_run(const mtkx_task_pipeline *pipeline,
                                               mtkx_phase_state *state);

/** @return true when the pipeline has no phases. */
MTKX_INLINE bool mtkx_task_pipeline_empty(const mtkx_task_pipeline *pipeline) {
  return pipeline->count == 0;
}
/** @return the number of phases. */
MTKX_INLINE size_t mtkx_task_pipeline_size(const mtkx_task_pipeline *pipeline) {
  return pipeline->count;
}

MTKX_END_DECLS

#endif /* MEMTKX_SCHEDULER_TASK_PIPELINE_H */
