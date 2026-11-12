#pragma once
/**
 * @file memtkx/space/immix_space.hpp
 * @brief `ImmixBlock` and `ImmixSpace` (wrap memtkx/space/immix_space.h).
 */

#include <cstddef>
#include <new>
#include <optional>
#include <vector>

#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/space/immix_space.h"
#include "memtkx/space/space.hpp"

namespace MemTKX {

inline constexpr std::size_t kImmixBlockBytes = MTKX_IMMIX_BLOCK_BYTES;
inline constexpr std::size_t kImmixLineBytes = MTKX_IMMIX_LINE_BYTES;
inline constexpr std::size_t kImmixLinesPerBlock = MTKX_IMMIX_LINES_PER_BLOCK;

/** One block with a cursor and line marks (a value type). */
class ImmixBlock {
 public:
  ImmixBlock(Address start, Address end) noexcept { mtkx_immix_block_init(&b_, start, end); }
  explicit ImmixBlock(const mtkx_immix_block& c) noexcept : b_(c) {}

  [[nodiscard]] std::optional<Address> allocate(std::size_t bytes, std::size_t alignment) {
    Address out = 0;
    if (mtkx_immix_block_allocate(&b_, bytes, alignment, &out)) {
      return out;
    }
    return std::nullopt;
  }

  void reset() noexcept { mtkx_immix_block_reset(&b_); }
  void clear_marks() noexcept { mtkx_immix_block_clear_marks(&b_); }
  void mark(Address address, std::size_t bytes) { mtkx_immix_block_mark(&b_, address, bytes); }

  [[nodiscard]] Address start() const noexcept { return b_.start; }
  [[nodiscard]] Address limit() const noexcept { return b_.limit; }
  [[nodiscard]] Address cursor() const noexcept { return b_.cursor; }
  [[nodiscard]] std::size_t used_bytes() const noexcept { return mtkx_immix_block_used_bytes(&b_); }
  [[nodiscard]] std::size_t remaining_bytes() const noexcept {
    return mtkx_immix_block_remaining_bytes(&b_);
  }
  [[nodiscard]] std::size_t marked_lines() const noexcept {
    return mtkx_immix_block_marked_lines(&b_);
  }
  [[nodiscard]] std::size_t marked_bytes() const noexcept {
    return mtkx_immix_block_marked_bytes(&b_);
  }

 private:
  mtkx_immix_block b_{};
};

class ImmixPlan;

/** Immix space owning its storage. */
class ImmixSpace final : private detail::CStorage<mtkx_immix_space>,
                         public SpaceBase<ImmixSpace> {
 public:
  explicit ImmixSpace(std::size_t bytes)
      : SpaceBase<ImmixSpace>(detail::adopt, &c_storage_.base), s_(&c_storage_), owned_(true) {
    detail::throw_if_nomem(mtkx_immix_space_init(&c_storage_, bytes));
  }
  ~ImmixSpace() {
    if (owned_) {
      mtkx_immix_space_destroy(&c_storage_);
    }
  }

  [[nodiscard]] AllocResult<Address> allocate_impl(std::size_t bytes, std::size_t alignment) {
    return detail::to_cpp(mtkx_immix_space_allocate(s_, bytes, alignment));
  }

  bool mark(Address address, std::size_t bytes) { return mtkx_immix_space_mark(s_, address, bytes); }
  void reset_blocks() noexcept { mtkx_immix_space_reset_blocks(s_); }
  void clear_marks() noexcept { mtkx_immix_space_clear_marks(s_); }

  [[nodiscard]] std::size_t block_count() const noexcept { return mtkx_immix_space_block_count(s_); }
  [[nodiscard]] std::size_t total_bytes() const noexcept { return mtkx_immix_space_total_bytes(s_); }
  [[nodiscard]] std::size_t used_bytes() const noexcept { return mtkx_immix_space_used_bytes(s_); }
  [[nodiscard]] std::size_t marked_bytes() const noexcept {
    return mtkx_immix_space_marked_bytes(s_);
  }

  /** Copies of the blocks (block state is owned by the C space). */
  [[nodiscard]] std::vector<ImmixBlock> blocks() const {
    std::vector<ImmixBlock> out;
    out.reserve(block_count());
    for (std::size_t i = 0; i < block_count(); ++i) {
      out.emplace_back(*mtkx_immix_space_block(s_, i));
    }
    return out;
  }

  [[nodiscard]] mtkx_immix_space* c_immix_space() noexcept { return s_; }

 private:
  friend class ImmixPlan;
  /** Non-owning view of a space owned elsewhere (an ImmixPlan's). */
  explicit ImmixSpace(mtkx_immix_space* external) noexcept
      : SpaceBase<ImmixSpace>(detail::adopt, &external->base), s_(external), owned_(false) {}

  mtkx_immix_space* s_;
  bool owned_;
};

}  // namespace MemTKX
