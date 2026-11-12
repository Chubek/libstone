#pragma once
/**
 * @file memtkx/core/error.hpp
 * @brief C++ error vocabularies and result aliases (wraps memtkx/core/error.h).
 */

#include <cstdint>
#include <string_view>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.h"
#include "memtkx/core/types.hpp"

namespace MemTKX {

enum class AllocError : std::uint8_t {
  OutOfMemory = MTKX_ALLOC_OUT_OF_MEMORY,
  Misaligned = MTKX_ALLOC_MISALIGNED,
  LargeObjectExceeded = MTKX_ALLOC_LARGE_OBJECT_EXCEEDED,
  AllocationFailed = MTKX_ALLOC_ALLOCATION_FAILED,
};

enum class GCError : std::uint8_t {
  MutatorNotRegistered = MTKX_GC_MUTATOR_NOT_REGISTERED,
  SafepointTimeout = MTKX_GC_SAFEPOINT_TIMEOUT,
  PlanUnavailable = MTKX_GC_PLAN_UNAVAILABLE,
  InvalidTrace = MTKX_GC_INVALID_TRACE,
  CoordinatorBusy = MTKX_GC_COORDINATOR_BUSY,
};

[[nodiscard]] inline std::string_view to_string(AllocError error) noexcept {
  return mtkx_alloc_error_string(static_cast<mtkx_alloc_error>(error));
}

[[nodiscard]] inline std::string_view to_string(GCError error) noexcept {
  return mtkx_gc_error_string(static_cast<mtkx_gc_error>(error));
}

template <typename T>
using AllocResult = dsl::Result<T, AllocError>;

template <typename T>
using GcResult = dsl::Result<T, GCError>;

namespace detail {

inline AllocResult<Address> to_cpp(mtkx_alloc_result r) {
  if (r.ok) {
    return AllocResult<Address>::from_ok(r.value);
  }
  return AllocResult<Address>::from_err(static_cast<AllocError>(r.error));
}

inline mtkx_alloc_result to_c(const AllocResult<Address>& r) {
  if (r.is_ok()) {
    return mtkx_alloc_result_from_ok(r.unwrap());
  }
  return mtkx_alloc_result_from_err(static_cast<mtkx_alloc_error>(r.unwrap_err()));
}

inline GcResult<int> to_cpp(mtkx_gc_result r) {
  if (r.ok) {
    return GcResult<int>::from_ok(r.value);
  }
  return GcResult<int>::from_err(static_cast<GCError>(r.error));
}

inline mtkx_gc_result to_c(const GcResult<int>& r) {
  if (r.is_ok()) {
    return mtkx_gc_result_from_ok(r.unwrap());
  }
  return mtkx_gc_result_from_err(static_cast<mtkx_gc_error>(r.unwrap_err()));
}

}  // namespace detail
}  // namespace MemTKX
