#pragma once
/**
 * @file memtkx/plan/plan.hpp
 * @brief `PlanBase<Derived>`, `PlanKind`, `PlanStats` and `TraceFn`
 *        (wrap memtkx/plan/plan.h).
 *
 * As before, a plan implements `kind_value()`, `name_value()` and
 * `collect_impl(roots, trace)`. Every plan also offers `c_plan()`, an
 * `mtkx_plan *` usable with mtkx_plan_allocate()/mtkx_plan_collect();
 * for application plans it dispatches back into the C++ methods.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/plan/plan.h"

namespace MemTKX {

enum class PlanKind : std::uint8_t {
  MarkSweep = MTKX_PLAN_MARK_SWEEP,
  SemiSpace = MTKX_PLAN_SEMI_SPACE,
  Immix = MTKX_PLAN_IMMIX,
  Generational = MTKX_PLAN_GENERATIONAL,
};

[[nodiscard]] inline std::string_view to_string(PlanKind kind) noexcept {
  return mtkx_plan_kind_string(static_cast<mtkx_plan_kind>(kind));
}

struct PlanStats {
  std::size_t live_objects{0};
  std::size_t live_bytes{0};
  std::size_t freed_objects{0};
  std::size_t freed_bytes{0};
  std::size_t collection_count{0};

  [[nodiscard]] static PlanStats from_c(const mtkx_plan_stats& s) noexcept {
    return {s.live_objects, s.live_bytes, s.freed_objects, s.freed_bytes, s.collection_count};
  }
  [[nodiscard]] mtkx_plan_stats to_c() const noexcept {
    return {live_objects, live_bytes, freed_objects, freed_bytes, collection_count};
  }
};

/** Object scanner: returns the addresses an object refers to. */
using TraceFn = std::function<std::vector<Address>(Address)>;

namespace detail {

inline bool trace_trampoline(mtkx_address object, mtkx_address_vec* children, void* ctx) {
  if (has_pending_exception()) {
    return false;
  }
  try {
    const std::vector<Address> refs = (*static_cast<const TraceFn*>(ctx))(object);
    return mtkx_address_vec_append(children, refs.data(), refs.size()) == MTKX_OK;
  } catch (...) {
    park_current_exception();
    return false;
  }
}

/** A C trace calling @p trace (which must outlive the call). */
inline mtkx_trace to_c_trace(const TraceFn& trace) noexcept {
  if (!trace) {
    return mtkx_trace_none();
  }
  return mtkx_trace_make(&trace_trampoline, const_cast<TraceFn*>(&trace));
}

/** Thrown inside a C++ collect_impl when a C trace callback fails. */
struct CTraceFailure {};

/** A TraceFn calling a C trace. */
inline TraceFn from_c_trace(mtkx_trace trace) {
  if (trace.fn == nullptr) {
    return TraceFn{};
  }
  return [trace](Address object) {
    mtkx_address_vec children = MTKX_ADDRESS_VEC_INIT;
    const bool ok = trace.fn(object, &children, trace.ctx);
    std::vector<Address> out(children.data, children.data + children.size);
    mtkx_address_vec_destroy(&children);
    if (!ok) {
      throw CTraceFailure{};
    }
    return out;
  };
}

inline GcResult<PlanStats> to_cpp(mtkx_plan_stats_result r) {
  if (r.ok) {
    return GcResult<PlanStats>::from_ok(PlanStats::from_c(r.value));
  }
  return GcResult<PlanStats>::from_err(static_cast<GCError>(r.error));
}

inline mtkx_plan_stats_result to_c(const GcResult<PlanStats>& r) {
  if (r.is_ok()) {
    return mtkx_plan_stats_result_from_ok(r.unwrap().to_c());
  }
  return mtkx_plan_stats_result_from_err(static_cast<mtkx_gc_error>(r.unwrap_err()));
}

/** Converts a C collection result, first rethrowing any trace exception. */
inline GcResult<PlanStats> finish_collect(mtkx_plan_stats_result r) {
  rethrow_pending();
  return to_cpp(r);
}

struct PlanHolder {
  mtkx_plan base; /* first member: mtkx_plan* <-> PlanHolder* */
  void* owner;
};

template <typename T, typename = void>
struct has_native_plan : std::false_type {};
template <typename T>
struct has_native_plan<T, std::void_t<decltype(std::declval<T&>().native_plan())>>
    : std::true_type {};

template <typename T, typename = void>
struct has_allocate : std::false_type {};
template <typename T>
struct has_allocate<T, std::void_t<decltype(std::declval<T&>().allocate(std::size_t{},
                                                                         std::size_t{}))>>
    : std::true_type {};

}  // namespace detail

template <typename Derived>
class PlanBase : public dsl::DSL<Derived, dsl::ResultFeature> {
 public:
  [[nodiscard]] PlanKind kind() const noexcept { return derived().kind_value(); }
  [[nodiscard]] std::string_view name() const noexcept { return derived().name_value(); }

  [[nodiscard]] GcResult<PlanStats> collect(const std::vector<Address>& roots,
                                            const TraceFn& trace) {
    return derived().collect_impl(roots, trace);
  }

  /**
   * @brief The C view of this plan. Built-in plans return their C plan;
   *        application plans get a C vtable dispatching to C++. The
   *        pointer is invalidated by moving the plan.
   */
  [[nodiscard]] mtkx_plan* c_plan() {
    if constexpr (detail::has_native_plan<Derived>::value) {
      return derived().native_plan();
    } else {
      if (!holder_) {
        holder_ = std::make_unique<detail::PlanHolder>();
        holder_->base.ops = &plugin_ops;
      }
      holder_->owner = this;
      return &holder_->base;
    }
  }

 private:
  const Derived& derived() const noexcept { return static_cast<const Derived&>(*this); }
  Derived& derived() noexcept { return static_cast<Derived&>(*this); }

  static PlanBase* owner_of(const mtkx_plan* plan) noexcept {
    return static_cast<PlanBase*>(reinterpret_cast<const detail::PlanHolder*>(plan)->owner);
  }

  static mtkx_plan_kind plugin_kind(const mtkx_plan* plan) {
    return static_cast<mtkx_plan_kind>(owner_of(plan)->derived().kind_value());
  }

  static const char* plugin_name(const mtkx_plan* plan) {
    PlanBase* self = owner_of(plan);
    self->c_name_ = std::string(self->derived().name_value());
    return self->c_name_.c_str();
  }

  static mtkx_alloc_result plugin_allocate(mtkx_plan* plan, std::size_t bytes,
                                           std::size_t alignment) {
    if constexpr (detail::has_allocate<Derived>::value) {
      try {
        return detail::to_c(owner_of(plan)->derived().allocate(bytes, alignment));
      } catch (...) {
        detail::park_current_exception();
      }
    }
    return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
  }

  static mtkx_plan_stats_result plugin_collect(mtkx_plan* plan, const mtkx_address* roots,
                                               std::size_t count, mtkx_trace trace) {
    try {
      const std::vector<Address> root_vec(roots, roots + count);
      return detail::to_c(owner_of(plan)->derived().collect_impl(root_vec,
                                                                 detail::from_c_trace(trace)));
    } catch (const detail::CTraceFailure&) {
      return mtkx_plan_stats_result_from_err(MTKX_GC_INVALID_TRACE);
    } catch (...) {
      detail::park_current_exception();
      return mtkx_plan_stats_result_from_err(MTKX_GC_PLAN_UNAVAILABLE);
    }
  }

  static inline const mtkx_plan_ops plugin_ops = {&PlanBase::plugin_kind, &PlanBase::plugin_name,
                                                  &PlanBase::plugin_allocate,
                                                  &PlanBase::plugin_collect, nullptr};

  std::unique_ptr<detail::PlanHolder> holder_;
  std::string c_name_;
};

}  // namespace MemTKX
