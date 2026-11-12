#pragma once
/**
 * @file memtkx/plan/immix.hpp
 * @brief `ImmixPlan` (wraps memtkx/plan/immix.h).
 */

#include <cstddef>
#include <memory>
#include <new>
#include <string_view>
#include <vector>

#include "memtkx/core/error.hpp"
#include "memtkx/plan/immix.h"
#include "memtkx/plan/plan.hpp"
#include "memtkx/space/immix_space.hpp"

namespace MemTKX {

/** Line-marking plan over an owned Immix space (move-only). */
class ImmixPlan : public PlanBase<ImmixPlan> {
 public:
  explicit ImmixPlan(std::size_t heap_bytes) : p_(mtkx_immix_plan_create(heap_bytes)) {
    if (!p_) {
      throw std::bad_alloc();
    }
    space_.reset(new ImmixSpace(const_cast<mtkx_immix_space*>(mtkx_immix_plan_space(p_.get()))));
  }

  [[nodiscard]] constexpr PlanKind kind_value() const noexcept { return PlanKind::Immix; }
  [[nodiscard]] constexpr std::string_view name_value() const noexcept { return "Immix"; }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes, std::size_t alignment) {
    return detail::to_cpp(mtkx_immix_plan_allocate(p_.get(), bytes, alignment));
  }

  bool mark(Address object) { return mtkx_immix_plan_mark(p_.get(), object); }

  [[nodiscard]] GcResult<PlanStats> collect_impl(const std::vector<Address>& roots,
                                                 const TraceFn& trace) {
    return detail::finish_collect(mtkx_immix_plan_collect(p_.get(), roots.data(), roots.size(),
                                                          detail::to_c_trace(trace)));
  }

  /** The plan's space, for observation. */
  [[nodiscard]] const ImmixSpace& space() const noexcept { return *space_; }

  [[nodiscard]] mtkx_plan* native_plan() noexcept { return mtkx_immix_plan_as_plan(p_.get()); }

 private:
  detail::Handle<mtkx_immix_plan, &mtkx_immix_plan_destroy> p_;
  std::unique_ptr<ImmixSpace> space_;
};

}  // namespace MemTKX
