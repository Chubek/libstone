/**
 * @file memtkx/dsl/task.h
 * @brief Named task chains with shared state (C++ `dsl::Task`,
 *        `dsl::TaskChain`, `dsl::run`, `dsl::suspend`, `dsl::upon`).
 *
 * This is the DSL's generic task chain. Garbage-collection phases use the
 * separate scheduler API in memtkx/scheduler/task_pipeline.h.
 *
 * @code
 *   static mtkx_task_result step(mtkx_task_state *st, void *ctx) {
 *     return mtkx_task_ok("done");
 *   }
 *
 *   mtkx_task_chain chain;
 *   mtkx_task_state state;
 *   mtkx_task_chain_init(&chain, MTKX_TASK_STOP_ON_ERROR);
 *   mtkx_task_chain_append(&chain, mtkx_task_make("step", step, NULL));
 *   mtkx_task_state_init(&state);
 *   mtkx_task_run(&state, &chain);
 *   // mtkx_task_state_find(&state, "step")->text == "done"
 *   mtkx_task_state_destroy(&state);
 *   mtkx_task_chain_destroy(&chain);
 * @endcode
 */
#ifndef MEMTKX_DSL_TASK_H
#define MEMTKX_DSL_TASK_H

#include <stddef.h>

#include "memtkx/core/base.h"

MTKX_BEGIN_DECLS

/** Ok or error text (C++ `Result<std::string, std::string>`). Owns `text`. */
typedef struct mtkx_task_result {
  bool ok;
  char *text; /**< allocated with mtkx_malloc; may be NULL after OOM */
} mtkx_task_result;

/** Ok result with a copy of @p text. */
MTKX_API mtkx_task_result mtkx_task_ok(const char *text);
/** Error result with a copy of @p text. */
MTKX_API mtkx_task_result mtkx_task_err(const char *text);
/** Releases the text of @p result. */
MTKX_API void mtkx_task_result_free(mtkx_task_result *result);

/** One named result stored in the state. */
typedef struct mtkx_task_record {
  char *name;
  mtkx_task_result result;
} mtkx_task_record;

/** Shared mutable state of a run (C++ `TaskState`). */
typedef struct mtkx_task_state {
  mtkx_task_record *records; /**< read-only for callers */
  size_t count;
  size_t capacity;
  bool suspended;
} mtkx_task_state;

/** Initialises an empty state. */
MTKX_API void mtkx_task_state_init(mtkx_task_state *state);
/** Releases all records. */
MTKX_API void mtkx_task_state_destroy(mtkx_task_state *state);
/** @return the result stored for @p name, or NULL. */
MTKX_API const mtkx_task_result *mtkx_task_state_find(const mtkx_task_state *state,
                                                      const char *name);
/** Stores (insert or replace) a result; ownership of @p result transfers. */
MTKX_API mtkx_status mtkx_task_state_set(mtkx_task_state *state, const char *name,
                                         mtkx_task_result result);

/** Task body: returns an owned result. */
typedef mtkx_task_result (*mtkx_task_fn)(mtkx_task_state *state, void *ctx);

/** A task (C++ `Task`). The name is borrowed here and copied by chains. */
typedef struct mtkx_task {
  const char *name;
  mtkx_task_fn fn;
  void *ctx;
} mtkx_task;

/** Builds a task. */
MTKX_INLINE mtkx_task mtkx_task_make(const char *name, mtkx_task_fn fn, void *ctx) {
  mtkx_task t;
  t.name = name;
  t.fn = fn;
  t.ctx = ctx;
  return t;
}

/** Runs a single task (a NULL fn yields an ok result with empty text). */
MTKX_API mtkx_task_result mtkx_task_invoke(const mtkx_task *task, mtkx_task_state *state);

/** Error policy of a chain. */
typedef enum mtkx_task_policy {
  MTKX_TASK_STOP_ON_ERROR = 0,
  MTKX_TASK_CONTINUE_ON_ERROR = 1
} mtkx_task_policy;

/** An ordered list of tasks (C++ `TaskChain`, built with `|`). */
typedef struct mtkx_task_chain {
  mtkx_task *tasks; /**< names owned by the chain */
  size_t count;
  size_t capacity;
  mtkx_task_policy policy;
} mtkx_task_chain;

/** Initialises an empty chain. */
MTKX_API void mtkx_task_chain_init(mtkx_task_chain *chain, mtkx_task_policy policy);
/** Appends a copy of @p task (C++ `chain | task`). */
MTKX_API mtkx_status mtkx_task_chain_append(mtkx_task_chain *chain, mtkx_task task);
/** Releases the chain. */
MTKX_API void mtkx_task_chain_destroy(mtkx_task_chain *chain);

/**
 * @brief Runs the chain (C++ `dsl::run`): each result is stored under its
 *        task name; stops after an error under STOP_ON_ERROR, or when the
 *        state becomes suspended.
 * @return MTKX_OK or MTKX_ERR_NOMEM (storing a result failed).
 */
MTKX_API mtkx_status mtkx_task_run(mtkx_task_state *state, const mtkx_task_chain *chain);

/** Marks the state suspended (C++ `dsl::suspend`). */
MTKX_API void mtkx_task_suspend(mtkx_task_state *state);

/** Event test for mtkx_task_upon(). */
typedef bool (*mtkx_task_event_fn)(void *ctx);

/** Runs @p task and stores its result when @p event returns true (C++ `upon`). */
MTKX_API mtkx_status mtkx_task_upon(mtkx_task_state *state, mtkx_task_event_fn event,
                                    void *event_ctx, mtkx_task task);

MTKX_END_DECLS

#endif /* MEMTKX_DSL_TASK_H */
