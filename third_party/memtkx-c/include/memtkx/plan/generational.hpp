#pragma once
/**
 * @file memtkx/plan/generational.hpp
 * @brief `GenerationalPlan` (wraps memtkx/plan/generational.h).
 */

#include <cstddef>
#include <new>
#include <string_view>
#include <vector>

#include "memtkx/core/error.hpp"
#include "memtkx/plan/generational.h"
#include "memtkx/plan/plan.hpp"

namespace MemTKX {

/** Nursery + mature heaps with a card table (move-only). */
class GenerationalPlan : public PlanBase<GenerationalPlan> {
 public:
  GenerationalPlan(std::size_t nursery_bytes, std::size_t mature_bytes)
      : p_(mtkx_generational_plan_create(nursery_bytes, mature_bytes)) {
    if (!p_) {
      throw std::bad_alloc();
    }
  }

  [[nodiscard]] constexpr PlanKind kind_value() const noexcept { return PlanKind::Generational; }
  [[nodiscard]] constexpr std::string_view name_value() const noexcept { return "Generational"; }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes, std::size_t alignment) {
    return detail::to_cpp(mtkx_generational_plan_allocate(p_.get(), bytes, alignment));
  }

  void remember(Address slot_address) { mtkx_generational_plan_remember(p_.get(), slot_address); }

  [[nodiscard]] std::size_t nursery_live_bytes() const noexcept {
    return mtkx_generational_plan_nursery_live_bytes(p_.get());
  }

  [[nodiscard]] std::size_t dirty_cards() const noexcept {
    return mtkx_generational_plan_dirty_cards(p_.get());
  }

  [[nodiscard]] GcResult<PlanStats> minor_collect(const std::vector<Address>& roots,
                                                  const TraceFn& trace) {
    return detail::finish_collect(mtkx_generational_plan_minor_collect(
        p_.get(), roots.data(), roots.size(), detail::to_c_trace(trace)));
  }

  [[nodiscard]] GcResult<PlanStats> collect_impl(const std::vector<Address>& roots,
                                                 const TraceFn& trace) {
    return minor_collect(roots, trace);
  }

  [[nodiscard]] mtkx_plan* native_plan() noexcept {
    return mtkx_generational_plan_as_plan(p_.get());
  }

 private:
  detail::Handle<mtkx_generational_plan, &mtkx_generational_plan_destroy> p_;
};

}  // namespace MemTKX
