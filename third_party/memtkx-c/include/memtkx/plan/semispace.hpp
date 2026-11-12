#pragma once
/**
 * @file memtkx/plan/semispace.hpp
 * @brief `SemiSpacePlan` (wraps memtkx/plan/semispace.h).
 */

#include <cstddef>
#include <new>
#include <string_view>
#include <vector>

#include "memtkx/core/error.hpp"
#include "memtkx/plan/plan.hpp"
#include "memtkx/plan/semispace.h"

namespace MemTKX {

/** Copying collector over two equal halves (move-only). */
class SemiSpacePlan : public PlanBase<SemiSpacePlan> {
 public:
  explicit SemiSpacePlan(std::size_t semi_space_bytes)
      : p_(mtkx_semi_space_plan_create(semi_space_bytes)) {
    if (!p_) {
      throw std::bad_alloc();
    }
  }

  [[nodiscard]] constexpr PlanKind kind_value() const noexcept { return PlanKind::SemiSpace; }
  [[nodiscard]] constexpr std::string_view name_value() const noexcept { return "SemiSpace"; }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes, std::size_t alignment) {
    return detail::to_cpp(mtkx_semi_space_plan_allocate(p_.get(), bytes, alignment));
  }

  [[nodiscard]] std::size_t live_objects() const noexcept {
    return mtkx_semi_space_plan_live_objects(p_.get());
  }

  [[nodiscard]] GcResult<PlanStats> collect_impl(const std::vector<Address>& roots,
                                                 const TraceFn& trace) {
    return detail::finish_collect(mtkx_semi_space_plan_collect(
        p_.get(), roots.data(), roots.size(), detail::to_c_trace(trace)));
  }

  [[nodiscard]] mtkx_plan* native_plan() noexcept { return mtkx_semi_space_plan_as_plan(p_.get()); }

 private:
  detail::Handle<mtkx_semi_space_plan, &mtkx_semi_space_plan_destroy> p_;
};

}  // namespace MemTKX
