#pragma once
/**
 * @file memtkx/space/space.hpp
 * @brief `SpaceBase<Derived>`: the space extension point, backed by a C
 *        `mtkx_space` (wraps memtkx/space/space.h).
 *
 * Built-in spaces wrap their C counterpart. An application space derives
 * from SpaceBase<Derived>, passes a SpaceDescriptor and implements
 * `allocate_impl(bytes, alignment)`, exactly as before. Every space also
 * exposes `c_space()`, an `mtkx_space *` that C code can allocate through;
 * for application spaces it dispatches back into `allocate_impl`.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/space/space.h"

namespace MemTKX {

enum class SpaceKind : std::uint8_t {
  BumpPointer = MTKX_SPACE_BUMP_POINTER,
  FreeList = MTKX_SPACE_FREE_LIST,
  Immix = MTKX_SPACE_IMMIX,
  LargeObject = MTKX_SPACE_LARGE_OBJECT,
  Nursery = MTKX_SPACE_NURSERY,
  SemiSpace = MTKX_SPACE_SEMI_SPACE,
};

[[nodiscard]] inline std::string_view to_string(SpaceKind kind) noexcept {
  return mtkx_space_kind_string(static_cast<mtkx_space_kind>(kind));
}

/** A snapshot of space accounting. */
struct SpaceMetrics {
  std::size_t allocated_bytes{0};
  std::size_t live_bytes{0};
  std::size_t total_allocations{0};
  std::size_t total_deallocations{0};
  std::size_t collection_count{0};

  void reset() noexcept { *this = SpaceMetrics{}; }

  [[nodiscard]] static SpaceMetrics from_c(const mtkx_space_metrics& m) noexcept {
    return {m.allocated_bytes, m.live_bytes, m.total_allocations, m.total_deallocations,
            m.collection_count};
  }
};

struct SpaceDescriptor {
  std::string name;
  SpaceKind kind{SpaceKind::BumpPointer};
  AddressRange region;
  std::size_t default_alignment{alignof(std::max_align_t)};
};

namespace detail {

/** Writable view of a C space's metrics (the protected `metrics_` member). */
struct MetricsAccess {
  std::size_t& allocated_bytes;
  std::size_t& live_bytes;
  std::size_t& total_allocations;
  std::size_t& total_deallocations;
  std::size_t& collection_count;

  explicit MetricsAccess(mtkx_space_metrics& m) noexcept
      : allocated_bytes(m.allocated_bytes),
        live_bytes(m.live_bytes),
        total_allocations(m.total_allocations),
        total_deallocations(m.total_deallocations),
        collection_count(m.collection_count) {}

  void reset() noexcept {
    allocated_bytes = 0;
    live_bytes = 0;
    total_allocations = 0;
    total_deallocations = 0;
    collection_count = 0;
  }
};

/** C space owned by an application (plugin) space. */
struct SpaceHolder {
  mtkx_space base; /* first member: mtkx_space* <-> SpaceHolder* */
  void* owner;
};

}  // namespace detail

template <typename Derived>
class SpaceBase : public dsl::DSL<Derived, dsl::ResultFeature> {
 public:
  SpaceBase(const SpaceBase&) = delete;
  SpaceBase& operator=(const SpaceBase&) = delete;

  [[nodiscard]] std::string_view name() const noexcept { return mtkx_space_name(space_); }
  [[nodiscard]] SpaceKind kind() const noexcept {
    return static_cast<SpaceKind>(mtkx_space_get_kind(space_));
  }
  [[nodiscard]] AddressRange region() const noexcept {
    return AddressRange::from_c(mtkx_space_region(space_));
  }
  [[nodiscard]] std::size_t default_alignment() const noexcept {
    return mtkx_space_default_alignment(space_);
  }
  [[nodiscard]] SpaceMetrics metrics() const noexcept {
    return SpaceMetrics::from_c(*mtkx_space_metrics_of(space_));
  }
  [[nodiscard]] bool contains(Address address) const noexcept {
    return mtkx_space_contains(space_, address);
  }

  /** Validates the alignment, then dispatches to the implementation. */
  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes, std::size_t alignment) {
    const mtkx_alloc_result r = mtkx_space_allocate(space_, bytes, alignment);
    detail::rethrow_pending();
    return detail::to_cpp(r);
  }

  /** The C view of this space (usable with every mtkx_space_* function). */
  [[nodiscard]] mtkx_space* c_space() noexcept { return space_; }
  [[nodiscard]] const mtkx_space* c_space() const noexcept { return space_; }

 protected:
  /** Application spaces: creates a C space dispatching to `allocate_impl`. */
  explicit SpaceBase(SpaceDescriptor descriptor)
      : holder_(std::make_unique<detail::SpaceHolder>()),
        space_(&holder_->base),
        metrics_(holder_->base.metrics) {
    holder_->owner = this;
    detail::throw_if_nomem(mtkx_space_init(
        space_, &plugin_ops, descriptor.name.c_str(),
        static_cast<mtkx_space_kind>(descriptor.kind), descriptor.region.to_c(),
        descriptor.default_alignment));
  }

  /** Built-in spaces: wraps a C space owned by the derived class. */
  SpaceBase(detail::adopt_t, mtkx_space* space) noexcept
      : space_(space), metrics_(space->metrics) {}

  ~SpaceBase() {
    if (holder_) {
      mtkx_space_destroy(&holder_->base);
    }
  }

  void record_allocation(std::size_t bytes) noexcept { mtkx_space_record_allocation(space_, bytes); }
  void record_deallocation(std::size_t bytes) noexcept {
    mtkx_space_record_deallocation(space_, bytes);
  }
  void record_collection() noexcept { mtkx_space_record_collection(space_); }
  void set_region(AddressRange region) noexcept { space_->descriptor.region = region.to_c(); }

 private:
  static mtkx_alloc_result plugin_allocate(mtkx_space* space, std::size_t bytes,
                                           std::size_t alignment) {
    auto* holder = reinterpret_cast<detail::SpaceHolder*>(space);
    try {
      auto* self = static_cast<SpaceBase*>(holder->owner);
      return detail::to_c(static_cast<Derived*>(self)->allocate_impl(bytes, alignment));
    } catch (...) {
      detail::park_current_exception();
      return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
    }
  }

  static inline const mtkx_space_ops plugin_ops = {&SpaceBase::plugin_allocate, nullptr};

  std::unique_ptr<detail::SpaceHolder> holder_;
  mtkx_space* space_;

 protected:
  /** Writable metrics of this space (C++ plugins: `metrics_.reset()`). */
  detail::MetricsAccess metrics_;
};

}  // namespace MemTKX
