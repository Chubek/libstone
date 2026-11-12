#include "memtkx/scheduler/task_pipeline.h"
#include "memtkx/scheduler/work_packet.h"

#include <string.h>

#include "internal/common.h"
#include "internal/platform.h"

/* ------------------------------------------------------------------ */
/*  Work queue (ring buffer)                                          */
/* ------------------------------------------------------------------ */

void mtkx_work_queue_init(mtkx_work_queue *queue) { memset(queue, 0, sizeof *queue); }

void mtkx_work_queue_destroy(mtkx_work_queue *queue) {
  mtkx_free(queue->data);
  mtkx_work_queue_init(queue);
}

static mtkx_status mtkx__work_queue_grow(mtkx_work_queue *queue) {
  size_t new_capacity = queue->capacity == 0 ? 16 : queue->capacity * 2;
  size_t i;
  mtkx_work_packet *data;
  if (queue->capacity > SIZE_MAX / 2 / sizeof(mtkx_work_packet)) {
    return MTKX_ERR_NOMEM;
  }
  data = (mtkx_work_packet *)mtkx_malloc(new_capacity * sizeof *data);
  if (data == NULL) {
    return MTKX_ERR_NOMEM;
  }
  for (i = 0; i < queue->size; ++i) {
    data[i] = queue->data[(queue->head + i) % queue->capacity];
  }
  mtkx_free(queue->data);
  queue->data = data;
  queue->capacity = new_capacity;
  queue->head = 0;
  return MTKX_OK;
}

mtkx_status mtkx_work_queue_push(mtkx_work_queue *queue, mtkx_work_packet packet) {
  if (queue->size == queue->capacity) {
    mtkx_status status = mtkx__work_queue_grow(queue);
    if (status != MTKX_OK) {
      return status;
    }
  }
  queue->data[(queue->head + queue->size) % queue->capacity] = packet;
  ++queue->size;
  return MTKX_OK;
}

bool mtkx_work_queue_try_pop(mtkx_work_queue *queue, mtkx_work_packet *packet) {
  if (queue->size == 0) {
    return false;
  }
  *packet = queue->data[queue->head];
  queue->head = (queue->head + 1) % queue->capacity;
  --queue->size;
  if (queue->size == 0) {
    queue->head = 0;
  }
  return true;
}

size_t mtkx_work_queue_drain(mtkx_work_queue *queue, mtkx_work_fn fn, void *ctx) {
  size_t processed = 0;
  mtkx_work_packet packet;
  while (mtkx_work_queue_try_pop(queue, &packet)) {
    if (fn != NULL) {
      fn(&packet, ctx);
    }
    ++processed;
  }
  return processed;
}

/* ------------------------------------------------------------------ */
/*  Phases and pipelines                                              */
/* ------------------------------------------------------------------ */

mtkx_gc_result mtkx_phase_run(const mtkx_phase *phase, mtkx_phase_state *state) {
  if (phase->fn == NULL) {
    return mtkx_gc_result_from_ok(0);
  }
  return phase->fn(state, phase->ctx);
}

void mtkx_task_pipeline_init(mtkx_task_pipeline *pipeline) {
  pipeline->phases = NULL;
  pipeline->count = 0;
  pipeline->capacity = 0;
}

void mtkx_task_pipeline_destroy(mtkx_task_pipeline *pipeline) {
  size_t i;
  for (i = 0; i < pipeline->count; ++i) {
    mtkx_free((char *)pipeline->phases[i].name);
  }
  mtkx_free(pipeline->phases);
  mtkx_task_pipeline_init(pipeline);
}

mtkx_status mtkx_task_pipeline_append(mtkx_task_pipeline *pipeline, mtkx_phase phase) {
  char *name = mtkx_strdup(phase.name);
  if (name == NULL ||
      MTKX__GROW(pipeline->phases, pipeline->capacity, pipeline->count + 1) != MTKX_OK) {
    mtkx_free(name);
    return MTKX_ERR_NOMEM;
  }
  phase.name = name;
  pipeline->phases[pipeline->count++] = phase;
  return MTKX_OK;
}

mtkx_status mtkx_task_pipeline_append_pipeline(mtkx_task_pipeline *pipeline,
                                               const mtkx_task_pipeline *other) {
  size_t i;
  size_t n = other->count; /* snapshot: appending a pipeline to itself is fine */
  for (i = 0; i < n; ++i) {
    mtkx_status status = mtkx_task_pipeline_append(pipeline, other->phases[i]);
    if (status != MTKX_OK) {
      return status;
    }
  }
  return MTKX_OK;
}

mtkx_gc_result mtkx_task_pipeline_run(const mtkx_task_pipeline *pipeline,
                                      mtkx_phase_state *state) {
  size_t i;
  state->completed_steps = 0;
  for (i = 0; i < pipeline->count; ++i) {
    const mtkx_phase *phase = &pipeline->phases[i];
    uint64_t start;
    mtkx_gc_result result;
    state->phase_name = phase->name;
    start = mtkx__monotonic_ns();
    result = mtkx_phase_run(phase, state);
    state->elapsed_ns = mtkx__monotonic_ns() - start;
    if (!result.ok && phase->stop_on_error) {
      return result;
    }
  }
  return mtkx_gc_result_from_ok((int)state->completed_steps);
}
