#pragma once
/**
 * @file memtkx/plan/marksweep.hpp
 * @brief `MarkSweepPlan` (wraps memtkx/plan/marksweep.h).
 */

#include <cstddef>
#include <new>
#include <string_view>
#include <vector>

#include "memtkx/core/error.hpp"
#include "memtkx/plan/marksweep.h"
#include "memtkx/plan/plan.hpp"

namespace MemTKX {

/** Non-moving mark-sweep over an owned free-list heap (move-only). */
class MarkSweepPlan : public PlanBase<MarkSweepPlan> {
 public:
  explicit MarkSweepPlan(std::size_t heap_bytes) : p_(mtkx_mark_sweep_plan_create(heap_bytes)) {
    if (!p_) {
      throw std::bad_alloc();
    }
  }

  [[nodiscard]] constexpr PlanKind kind_value() const noexcept { return PlanKind::MarkSweep; }
  [[nodiscard]] constexpr std::string_view name_value() const noexcept { return "MarkSweep"; }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes, std::size_t alignment) {
    return detail::to_cpp(mtkx_mark_sweep_plan_allocate(p_.get(), bytes, alignment));
  }

  bool mark(Address object) { return mtkx_mark_sweep_plan_mark(p_.get(), object); }
  [[nodiscard]] bool is_marked(Address object) const noexcept {
    return mtkx_mark_sweep_plan_is_marked(p_.get(), object);
  }
  [[nodiscard]] std::size_t live_objects() const noexcept {
    return mtkx_mark_sweep_plan_live_objects(p_.get());
  }
  [[nodiscard]] std::size_t live_bytes() const noexcept {
    return mtkx_mark_sweep_plan_live_bytes(p_.get());
  }

  [[nodiscard]] GcResult<PlanStats> collect_impl(const std::vector<Address>& roots,
                                                 const TraceFn& trace) {
    return detail::finish_collect(mtkx_mark_sweep_plan_collect(
        p_.get(), roots.data(), roots.size(), detail::to_c_trace(trace)));
  }

  [[nodiscard]] mtkx_plan* native_plan() noexcept { return mtkx_mark_sweep_plan_as_plan(p_.get()); }
  [[nodiscard]] mtkx_mark_sweep_plan* c_mark_sweep() const noexcept { return p_.get(); }

 private:
  detail::Handle<mtkx_mark_sweep_plan, &mtkx_mark_sweep_plan_destroy> p_;
};

}  // namespace MemTKX
