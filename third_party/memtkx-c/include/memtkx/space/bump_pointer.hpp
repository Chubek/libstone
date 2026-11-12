#pragma once
/**
 * @file memtkx/space/bump_pointer.hpp
 * @brief `BumpPointerAllocator` and `BumpSpace` (wrap memtkx/space/bump_pointer.h).
 */

#include <cstddef>
#include <new>
#include <string>

#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/space/bump_pointer.h"
#include "memtkx/space/space.hpp"

namespace MemTKX {

struct BumpPointerPolicy {
  static constexpr SpaceKind kind = SpaceKind::BumpPointer;
};

/** Linear allocation over borrowed storage; a copyable value type. */
class BumpPointerAllocator : public dsl::DSL<BumpPointerAllocator, dsl::ResultFeature> {
 public:
  BumpPointerAllocator() noexcept = default;
  BumpPointerAllocator(Address start, Address end) noexcept {
    mtkx_bump_allocator_init(&a_, start, end);
  }
  explicit BumpPointerAllocator(const mtkx_bump_allocator& c) noexcept : a_(c) {}

  /** Uses the inline fast path; the full C call supplies the error reason. */
  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes, std::size_t alignment) {
    const Address fast = mtkx_bump_allocator_try_allocate(&a_, bytes, alignment);
    if (fast != 0) {
      return AllocResult<Address>::from_ok(fast);
    }
    return detail::to_cpp(mtkx_bump_allocator_allocate(&a_, bytes, alignment));
  }

  void reset() noexcept { mtkx_bump_allocator_reset(&a_); }
  void reset(Address start, Address end) noexcept {
    mtkx_bump_allocator_reset_region(&a_, start, end);
  }

  [[nodiscard]] Address start() const noexcept { return mtkx_bump_allocator_start(&a_); }
  [[nodiscard]] Address limit() const noexcept { return mtkx_bump_allocator_limit(&a_); }
  [[nodiscard]] Address cursor() const noexcept { return mtkx_bump_allocator_cursor(&a_); }
  [[nodiscard]] std::size_t remaining() const noexcept { return mtkx_bump_allocator_remaining(&a_); }
  [[nodiscard]] std::size_t used_bytes() const noexcept {
    return mtkx_bump_allocator_used_bytes(&a_);
  }
  [[nodiscard]] std::size_t allocation_count() const noexcept {
    return mtkx_bump_allocator_allocation_count(&a_);
  }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return mtkx_bump_allocator_allocated_bytes(&a_);
  }

  [[nodiscard]] mtkx_bump_allocator* c_allocator() noexcept { return &a_; }
  [[nodiscard]] const mtkx_bump_allocator* c_allocator() const noexcept { return &a_; }

 private:
  mtkx_bump_allocator a_{};
};

/** Bump allocation with space metadata. */
class BumpSpace final : private detail::CStorage<mtkx_bump_space>,
                        public SpaceBase<BumpSpace> {
 public:
  BumpSpace(std::string name, Address start, Address end)
      : SpaceBase<BumpSpace>(detail::adopt, &c_storage_.base) {
    detail::throw_if_nomem(mtkx_bump_space_init(&c_storage_, name.c_str(), start, end));
  }
  ~BumpSpace() { mtkx_bump_space_destroy(&c_storage_); }

  [[nodiscard]] AllocResult<Address> allocate_impl(std::size_t bytes, std::size_t alignment) {
    return detail::to_cpp(mtkx_bump_space_allocate(&c_storage_, bytes, alignment));
  }

  void reset() noexcept { mtkx_bump_space_reset(&c_storage_); }

  /** A snapshot of the underlying allocator. */
  [[nodiscard]] BumpPointerAllocator allocator() const noexcept {
    return BumpPointerAllocator(c_storage_.allocator);
  }
};

}  // namespace MemTKX
