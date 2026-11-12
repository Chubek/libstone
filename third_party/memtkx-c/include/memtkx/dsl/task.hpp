#pragma once
/**
 * @file memtkx/dsl/task.hpp
 * @brief The DSL task chain (`Task`, `TaskChain`, `run`, `suspend`,
 *        `upon`); `run` drives the chain through mtkx_task_run().
 */

#include <functional>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "memtkx/dsl/result.hpp"
#include "memtkx/dsl/task.h"

namespace MemTKX::dsl {

enum class TaskPolicy { StopOnError = MTKX_TASK_STOP_ON_ERROR, ContinueOnError = MTKX_TASK_CONTINUE_ON_ERROR };

struct TaskState {
  std::unordered_map<std::string, Result<std::string, std::string>> results{};
  bool suspended{false};
};

struct Task {
  std::string name{};
  std::function<Result<std::string, std::string>(TaskState&)> run_fn{};
  Result<std::string, std::string> run(TaskState& state) const { return run_fn(state); }
};

struct TaskChain {
  std::vector<Task> tasks{};
  TaskPolicy policy{TaskPolicy::StopOnError};
};

inline TaskChain operator|(Task lhs, Task rhs) {
  return TaskChain{{std::move(lhs), std::move(rhs)}, TaskPolicy::StopOnError};
}

inline TaskChain operator|(TaskChain lhs, Task rhs) {
  lhs.tasks.push_back(std::move(rhs));
  return lhs;
}

namespace detail {
struct TaskRunContext {
  const Task* task;
  TaskState* state;
  mtkx_task_state* c_state;
};

inline mtkx_task_result task_trampoline(mtkx_task_state* c_state, void* ctx) {
  auto* c = static_cast<TaskRunContext*>(ctx);
  try {
    Result<std::string, std::string> r = c->task->run(*c->state);
    c->state->results.insert_or_assign(c->task->name, r);
    c_state->suspended = c->state->suspended;
    return r.is_ok() ? mtkx_task_ok(r.unwrap().c_str()) : mtkx_task_err(r.unwrap_err().c_str());
  } catch (...) {
    MemTKX::detail::park_current_exception();
    c_state->suspended = true; /* stop the chain; the exception is rethrown */
    return mtkx_task_err("exception");
  }
}
}  // namespace detail

/** Runs @p chain, storing each result under its task name. */
template <typename... Args>
TaskState& run(TaskState& state, const TaskChain& chain, Args&&...) {
  mtkx_task_chain c_chain;
  mtkx_task_state c_state;
  std::vector<detail::TaskRunContext> contexts;
  contexts.reserve(chain.tasks.size());
  mtkx_task_chain_init(&c_chain, static_cast<mtkx_task_policy>(chain.policy));
  mtkx_task_state_init(&c_state);
  c_state.suspended = state.suspended;

  mtkx_status status = MTKX_OK;
  for (const Task& task : chain.tasks) {
    contexts.push_back({&task, &state, &c_state});
    status = mtkx_task_chain_append(
        &c_chain, mtkx_task_make(task.name.c_str(), &detail::task_trampoline, &contexts.back()));
    if (status != MTKX_OK) {
      break;
    }
  }
  if (status == MTKX_OK) {
    status = mtkx_task_run(&c_state, &c_chain);
  }
  mtkx_task_state_destroy(&c_state);
  mtkx_task_chain_destroy(&c_chain);
  MemTKX::detail::rethrow_pending();
  MemTKX::detail::throw_if_nomem(status);
  return state;
}

inline void suspend(TaskState& state) { state.suspended = true; }

template <typename EventFn>
void upon(TaskState& state, EventFn&& event, Task task) {
  if (event()) {
    state.results.insert_or_assign(task.name, task.run(state));
  }
}

}  // namespace MemTKX::dsl
