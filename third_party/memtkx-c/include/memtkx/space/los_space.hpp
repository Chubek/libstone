#pragma once
/**
 * @file memtkx/space/los_space.hpp
 * @brief `LargeObjectSpace` (wraps memtkx/space/los_space.h).
 */

#include <cstddef>
#include <string>

#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/space/bump_pointer.hpp"
#include "memtkx/space/los_space.h"
#include "memtkx/space/space.hpp"

namespace MemTKX {

inline constexpr std::size_t kDefaultLargeObjectThreshold = MTKX_DEFAULT_LARGE_OBJECT_THRESHOLD;

class LargeObjectSpace final : private detail::CStorage<mtkx_los_space>,
                               public SpaceBase<LargeObjectSpace> {
 public:
  LargeObjectSpace(std::string name, Address start, Address end,
                   std::size_t threshold = kDefaultLargeObjectThreshold)
      : SpaceBase<LargeObjectSpace>(detail::adopt, &c_storage_.base) {
    detail::throw_if_nomem(mtkx_los_space_init(&c_storage_, name.c_str(), start, end, threshold));
  }
  ~LargeObjectSpace() { mtkx_los_space_destroy(&c_storage_); }

  [[nodiscard]] AllocResult<Address> allocate_impl(std::size_t bytes, std::size_t alignment) {
    return detail::to_cpp(mtkx_los_space_allocate(&c_storage_, bytes, alignment));
  }

  bool deallocate(Address address, std::size_t bytes) noexcept {
    return mtkx_los_space_deallocate(&c_storage_, address, bytes);
  }

  void reset() noexcept { mtkx_los_space_reset(&c_storage_); }

  [[nodiscard]] std::size_t threshold() const noexcept {
    return mtkx_los_space_threshold(&c_storage_);
  }

  /** A snapshot of the underlying bump allocator. */
  [[nodiscard]] BumpPointerAllocator allocator() const noexcept {
    return BumpPointerAllocator(c_storage_.allocator);
  }
};

}  // namespace MemTKX
