#pragma once
/**
 * @file memtkx/space/free_list.hpp
 * @brief `FreeListAllocator`, `FreeListSpace` and
 *        `SegregatedFreeListAllocator` (wrap memtkx/space/free_list.h).
 */

#include <array>
#include <cstddef>
#include <new>
#include <string>
#include <utility>

#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/space/free_list.h"
#include "memtkx/space/space.hpp"

namespace MemTKX {

/** Read-only view of a C free list (what `FreeListSpace::allocator()` returns). */
class FreeListAllocatorView {
 public:
  explicit FreeListAllocatorView(const mtkx_free_list_allocator* a) noexcept : a_(a) {}

  [[nodiscard]] Address start() const noexcept { return mtkx_free_list_allocator_start(a_); }
  [[nodiscard]] Address limit() const noexcept { return mtkx_free_list_allocator_limit(a_); }
  [[nodiscard]] bool contains(Address address) const noexcept {
    return mtkx_free_list_allocator_contains(a_, address);
  }
  [[nodiscard]] std::size_t free_cells() const noexcept {
    return mtkx_free_list_allocator_free_cells(a_);
  }
  [[nodiscard]] std::size_t free_bytes() const noexcept {
    return mtkx_free_list_allocator_free_bytes(a_);
  }
  [[nodiscard]] std::size_t allocation_count() const noexcept {
    return mtkx_free_list_allocator_allocation_count(a_);
  }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return mtkx_free_list_allocator_allocated_bytes(a_);
  }

 private:
  const mtkx_free_list_allocator* a_;
};

/** First-fit, address-ordered free list over borrowed storage (move-only). */
class FreeListAllocator : public dsl::DSL<FreeListAllocator, dsl::ResultFeature> {
 public:
  struct FreeCell {
    Address address{0};
    std::size_t size{0};
    [[nodiscard]] constexpr Address end() const noexcept { return address + size; }
  };

  FreeListAllocator() noexcept = default;
  FreeListAllocator(Address start, Address end) { reset(start, end); }
  FreeListAllocator(const FreeListAllocator&) = delete;
  FreeListAllocator& operator=(const FreeListAllocator&) = delete;
  FreeListAllocator(FreeListAllocator&& other) noexcept
      : a_(std::exchange(other.a_, mtkx_free_list_allocator{})) {}
  FreeListAllocator& operator=(FreeListAllocator&& other) noexcept {
    if (this != &other) {
      mtkx_free_list_allocator_destroy(&a_);
      a_ = std::exchange(other.a_, mtkx_free_list_allocator{});
    }
    return *this;
  }
  ~FreeListAllocator() { mtkx_free_list_allocator_destroy(&a_); }

  void reset(Address start, Address end) {
    detail::throw_if_nomem(mtkx_free_list_allocator_reset(&a_, start, end));
  }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes, std::size_t alignment) {
    return detail::to_cpp(mtkx_free_list_allocator_allocate(&a_, bytes, alignment));
  }

  bool free(Address address, std::size_t bytes) {
    return mtkx_free_list_allocator_free(&a_, address, bytes);
  }

  [[nodiscard]] FreeListAllocatorView view() const noexcept { return FreeListAllocatorView(&a_); }
  [[nodiscard]] Address start() const noexcept { return view().start(); }
  [[nodiscard]] Address limit() const noexcept { return view().limit(); }
  [[nodiscard]] bool contains(Address address) const noexcept { return view().contains(address); }
  [[nodiscard]] std::size_t free_cells() const noexcept { return view().free_cells(); }
  [[nodiscard]] std::size_t free_bytes() const noexcept { return view().free_bytes(); }
  [[nodiscard]] std::size_t allocation_count() const noexcept { return view().allocation_count(); }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept { return view().allocated_bytes(); }

  [[nodiscard]] mtkx_free_list_allocator* c_allocator() noexcept { return &a_; }

 private:
  mtkx_free_list_allocator a_{};
};

/** Free list with space metadata. */
class FreeListSpace final : private detail::CStorage<mtkx_free_list_space>,
                            public SpaceBase<FreeListSpace> {
 public:
  FreeListSpace(std::string name, Address start, Address end)
      : SpaceBase<FreeListSpace>(detail::adopt, &c_storage_.base) {
    detail::throw_if_nomem(mtkx_free_list_space_init(&c_storage_, name.c_str(), start, end));
  }
  ~FreeListSpace() { mtkx_free_list_space_destroy(&c_storage_); }

  [[nodiscard]] AllocResult<Address> allocate_impl(std::size_t bytes, std::size_t alignment) {
    return detail::to_cpp(mtkx_free_list_space_allocate(&c_storage_, bytes, alignment));
  }

  bool deallocate(Address address, std::size_t bytes) {
    return mtkx_free_list_space_deallocate(&c_storage_, address, bytes);
  }

  void reset() { detail::throw_if_nomem(mtkx_free_list_space_reset(&c_storage_)); }

  [[nodiscard]] FreeListAllocatorView allocator() const noexcept {
    return FreeListAllocatorView(&c_storage_.allocator);
  }
};

/** Eight size-class partitions of one region (move-only). */
class SegregatedFreeListAllocator
    : public dsl::DSL<SegregatedFreeListAllocator, dsl::ResultFeature> {
 public:
  static constexpr std::array<std::size_t, MTKX_SEGREGATED_CLASS_COUNT> kSizeClasses{
      16, 32, 64, 128, 256, 512, 1024, 4096};

  SegregatedFreeListAllocator() noexcept = default;
  SegregatedFreeListAllocator(Address start, Address end) { reset(start, end); }
  SegregatedFreeListAllocator(const SegregatedFreeListAllocator&) = delete;
  SegregatedFreeListAllocator& operator=(const SegregatedFreeListAllocator&) = delete;
  SegregatedFreeListAllocator(SegregatedFreeListAllocator&& other) noexcept
      : a_(std::exchange(other.a_, mtkx_segregated_allocator{})) {}
  SegregatedFreeListAllocator& operator=(SegregatedFreeListAllocator&& other) noexcept {
    if (this != &other) {
      mtkx_segregated_allocator_destroy(&a_);
      a_ = std::exchange(other.a_, mtkx_segregated_allocator{});
    }
    return *this;
  }
  ~SegregatedFreeListAllocator() { mtkx_segregated_allocator_destroy(&a_); }

  void reset(Address start, Address end) {
    detail::throw_if_nomem(mtkx_segregated_allocator_reset(&a_, start, end));
  }

  [[nodiscard]] AllocResult<Address> allocate(std::size_t bytes, std::size_t alignment) {
    return detail::to_cpp(mtkx_segregated_allocator_allocate(&a_, bytes, alignment));
  }

  bool free(Address address, std::size_t bytes) {
    return mtkx_segregated_allocator_free(&a_, address, bytes);
  }

  [[nodiscard]] Address start() const noexcept { return mtkx_segregated_allocator_start(&a_); }
  [[nodiscard]] Address limit() const noexcept { return mtkx_segregated_allocator_limit(&a_); }
  [[nodiscard]] constexpr std::size_t class_count() const noexcept { return kSizeClasses.size(); }

  [[nodiscard]] mtkx_segregated_allocator* c_allocator() noexcept { return &a_; }

 private:
  mtkx_segregated_allocator a_{};
};

}  // namespace MemTKX
