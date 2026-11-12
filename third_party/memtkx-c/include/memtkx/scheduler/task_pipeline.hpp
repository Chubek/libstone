#pragma once
/**
 * @file memtkx/scheduler/task_pipeline.hpp
 * @brief Collection phases (`Phase`, `TaskPipeline`, `PhaseState`,
 *        `make_phase`, `|`), executed by mtkx_task_pipeline_run().
 */

#include <chrono>
#include <cstddef>
#include <functional>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/scheduler/task_pipeline.h"

namespace MemTKX {

struct PhaseState {
  std::string phase_name;
  std::size_t completed_steps{0};
  std::chrono::steady_clock::duration elapsed{};

  void note_completion(std::size_t steps = 1) noexcept { completed_steps += steps; }
};

class Phase {
 public:
  using Function = std::function<GcResult<int>(PhaseState&)>;

  Phase(std::string name, Function function, bool stop_on_error = true)
      : name_(std::move(name)), function_(std::move(function)), stop_on_error_(stop_on_error) {}

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] bool stop_on_error() const noexcept { return stop_on_error_; }

  [[nodiscard]] GcResult<int> run(PhaseState& state) {
    return function_ ? function_(state) : GcResult<int>::from_ok(0);
  }

 private:
  std::string name_;
  Function function_;
  bool stop_on_error_;
};

namespace detail {
struct PhaseRunContext {
  Phase* phase;
  PhaseState* state;
};

inline void sync_from_c(PhaseState& s, const mtkx_phase_state& c) {
  if (c.phase_name != nullptr) {
    s.phase_name = c.phase_name;
  }
  s.completed_steps = c.completed_steps;
  s.elapsed = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::nanoseconds(c.elapsed_ns));
}

inline mtkx_gc_result phase_trampoline(mtkx_phase_state* c_state, void* ctx) {
  auto* run = static_cast<PhaseRunContext*>(ctx);
  if (has_pending_exception()) {
    return mtkx_gc_result_from_err(MTKX_GC_PLAN_UNAVAILABLE);
  }
  try {
    sync_from_c(*run->state, *c_state);
    const GcResult<int> r = run->phase->run(*run->state);
    c_state->completed_steps = run->state->completed_steps;
    return to_c(r);
  } catch (...) {
    park_current_exception();
    return mtkx_gc_result_from_err(MTKX_GC_PLAN_UNAVAILABLE);
  }
}
}  // namespace detail

class TaskPipeline {
 public:
  TaskPipeline() = default;
  explicit TaskPipeline(Phase phase) { phases_.push_back(std::move(phase)); }

  TaskPipeline& append(Phase phase) {
    phases_.push_back(std::move(phase));
    return *this;
  }

  TaskPipeline& append(TaskPipeline other) {
    for (auto& phase : other.phases_) {
      phases_.push_back(std::move(phase));
    }
    return *this;
  }

  /** Runs every phase in order through the C scheduler. */
  [[nodiscard]] GcResult<int> run(PhaseState& state) {
    mtkx_task_pipeline c;
    mtkx_task_pipeline_init(&c);
    std::vector<detail::PhaseRunContext> contexts;
    contexts.reserve(phases_.size());
    for (auto& phase : phases_) {
      contexts.push_back({&phase, &state});
      if (mtkx_task_pipeline_append(&c, mtkx_make_phase(phase.name().c_str(),
                                                        &detail::phase_trampoline,
                                                        &contexts.back(),
                                                        phase.stop_on_error())) != MTKX_OK) {
        mtkx_task_pipeline_destroy(&c);
        throw std::bad_alloc();
      }
    }
    mtkx_phase_state c_state;
    mtkx_phase_state_init(&c_state);
    c_state.completed_steps = state.completed_steps;
    const mtkx_gc_result r = mtkx_task_pipeline_run(&c, &c_state);
    detail::sync_from_c(state, c_state); /* before the C names are released */
    mtkx_task_pipeline_destroy(&c);
    detail::rethrow_pending();
    return detail::to_cpp(r);
  }

  [[nodiscard]] bool empty() const noexcept { return phases_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return phases_.size(); }

 private:
  std::vector<Phase> phases_;
};

[[nodiscard]] inline TaskPipeline operator|(TaskPipeline lhs, Phase rhs) {
  lhs.append(std::move(rhs));
  return lhs;
}

[[nodiscard]] inline TaskPipeline operator|(TaskPipeline lhs, TaskPipeline rhs) {
  lhs.append(std::move(rhs));
  return lhs;
}

[[nodiscard]] inline TaskPipeline operator|(Phase lhs, Phase rhs) {
  TaskPipeline pipeline(std::move(lhs));
  pipeline.append(std::move(rhs));
  return pipeline;
}

[[nodiscard]] inline TaskPipeline operator|(Phase lhs, TaskPipeline rhs) {
  TaskPipeline pipeline(std::move(lhs));
  pipeline.append(std::move(rhs));
  return pipeline;
}

[[nodiscard]] inline Phase make_phase(std::string name, Phase::Function function,
                                      bool stop_on_error = true) {
  return Phase(std::move(name), std::move(function), stop_on_error);
}

}  // namespace MemTKX
