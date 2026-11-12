#include "memtkx/dsl/task.h"

#include <string.h>

#include "internal/common.h"

mtkx_task_result mtkx_task_ok(const char *text) {
  mtkx_task_result r;
  r.ok = true;
  r.text = mtkx_strdup(text);
  return r;
}

mtkx_task_result mtkx_task_err(const char *text) {
  mtkx_task_result r;
  r.ok = false;
  r.text = mtkx_strdup(text);
  return r;
}

void mtkx_task_result_free(mtkx_task_result *result) {
  mtkx_free(result->text);
  result->text = NULL;
}

void mtkx_task_state_init(mtkx_task_state *state) {
  state->records = NULL;
  state->count = 0;
  state->capacity = 0;
  state->suspended = false;
}

void mtkx_task_state_destroy(mtkx_task_state *state) {
  size_t i;
  for (i = 0; i < state->count; ++i) {
    mtkx_free(state->records[i].name);
    mtkx_task_result_free(&state->records[i].result);
  }
  mtkx_free(state->records);
  mtkx_task_state_init(state);
}

static mtkx_task_record *mtkx__task_find(const mtkx_task_state *state, const char *name) {
  size_t i;
  for (i = 0; i < state->count; ++i) {
    if (strcmp(state->records[i].name, name) == 0) {
      return &state->records[i];
    }
  }
  return NULL;
}

const mtkx_task_result *mtkx_task_state_find(const mtkx_task_state *state,
                                             const char *name) {
  mtkx_task_record *record = mtkx__task_find(state, name != NULL ? name : "");
  return record != NULL ? &record->result : NULL;
}

mtkx_status mtkx_task_state_set(mtkx_task_state *state, const char *name,
                                mtkx_task_result result) {
  mtkx_task_record *record;
  char *copy;
  if (name == NULL) {
    name = "";
  }
  record = mtkx__task_find(state, name);
  if (record != NULL) {
    mtkx_task_result_free(&record->result);
    record->result = result;
    return MTKX_OK;
  }
  copy = mtkx_strdup(name);
  if (copy == NULL ||
      MTKX__GROW(state->records, state->capacity, state->count + 1) != MTKX_OK) {
    mtkx_free(copy);
    mtkx_task_result_free(&result);
    return MTKX_ERR_NOMEM;
  }
  state->records[state->count].name = copy;
  state->records[state->count].result = result;
  ++state->count;
  return MTKX_OK;
}

mtkx_task_result mtkx_task_invoke(const mtkx_task *task, mtkx_task_state *state) {
  if (task->fn == NULL) {
    return mtkx_task_ok("");
  }
  return task->fn(state, task->ctx);
}

void mtkx_task_chain_init(mtkx_task_chain *chain, mtkx_task_policy policy) {
  chain->tasks = NULL;
  chain->count = 0;
  chain->capacity = 0;
  chain->policy = policy;
}

mtkx_status mtkx_task_chain_append(mtkx_task_chain *chain, mtkx_task task) {
  char *name = mtkx_strdup(task.name);
  if (name == NULL ||
      MTKX__GROW(chain->tasks, chain->capacity, chain->count + 1) != MTKX_OK) {
    mtkx_free(name);
    return MTKX_ERR_NOMEM;
  }
  task.name = name;
  chain->tasks[chain->count++] = task;
  return MTKX_OK;
}

void mtkx_task_chain_destroy(mtkx_task_chain *chain) {
  size_t i;
  for (i = 0; i < chain->count; ++i) {
    mtkx_free((char *)chain->tasks[i].name);
  }
  mtkx_free(chain->tasks);
  mtkx_task_chain_init(chain, chain->policy);
}

mtkx_status mtkx_task_run(mtkx_task_state *state, const mtkx_task_chain *chain) {
  size_t i;
  for (i = 0; i < chain->count; ++i) {
    const mtkx_task *task = &chain->tasks[i];
    mtkx_task_result result = mtkx_task_invoke(task, state);
    bool failed = !result.ok;
    mtkx_status status = mtkx_task_state_set(state, task->name, result);
    if (status != MTKX_OK) {
      return status;
    }
    if (failed && chain->policy == MTKX_TASK_STOP_ON_ERROR) {
      break;
    }
    if (state->suspended) {
      break;
    }
  }
  return MTKX_OK;
}

void mtkx_task_suspend(mtkx_task_state *state) { state->suspended = true; }

mtkx_status mtkx_task_upon(mtkx_task_state *state, mtkx_task_event_fn event,
                           void *event_ctx, mtkx_task task) {
  if (event != NULL && event(event_ctx)) {
    return mtkx_task_state_set(state, task.name, mtkx_task_invoke(&task, state));
  }
  return MTKX_OK;
}
