#pragma once
/**
 * @file memtkx/barrier/barrier.hpp
 * @brief Barrier pipeline stages (wrap memtkx/barrier/barrier.h).
 *
 * `Slot` and `ReadSlot` *are* the C structs, so slots pass freely between
 * C and C++. The filter stages evaluate their predicates with
 * mtkx_barrier_run(); `SlotBuffer` is an RAII owner of a C slot buffer.
 */

#include <cstddef>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/barrier/barrier.h"
#include "memtkx/core/types.hpp"

namespace MemTKX {

/** `{Address* slot_address; Address target_object;}` — the C type. */
using Slot = mtkx_slot;
/** `{const Address* slot_address; Address loaded_object;}` — the C type. */
using ReadSlot = mtkx_read_slot;

struct BarrierDSL : dsl::DSL<BarrierDSL, dsl::Pipeline> {};

namespace detail {
inline std::optional<Slot> run_stage(const mtkx_barrier_stage& stage, Slot slot) {
  if (mtkx_barrier_run(slot, &stage, 1)) {
    return slot;
  }
  return std::nullopt;
}
}  // namespace detail

[[nodiscard]] inline auto barrier_non_null() {
  return dsl::pipe([stage = mtkx_barrier_non_null()](Slot slot) -> std::optional<Slot> {
    return detail::run_stage(stage, slot);
  });
}

[[nodiscard]] inline auto barrier_filter_range(AddressRange range) {
  return dsl::pipe(
      [stage = mtkx_barrier_filter_range(range.to_c())](Slot slot) -> std::optional<Slot> {
        return detail::run_stage(stage, slot);
      });
}

[[nodiscard]] inline auto barrier_filter_target_range(AddressRange range) {
  return dsl::pipe(
      [stage = mtkx_barrier_filter_target_range(range.to_c())](Slot slot) -> std::optional<Slot> {
        return detail::run_stage(stage, slot);
      });
}

[[nodiscard]] inline auto barrier_cross_region(AddressRange source_region,
                                               AddressRange target_region) {
  return dsl::pipe([stage = mtkx_barrier_cross_region(source_region.to_c(),
                                                      target_region.to_c())](Slot slot)
                       -> std::optional<Slot> { return detail::run_stage(stage, slot); });
}

[[nodiscard]] inline auto barrier_saturating_filter(AddressRange source_region,
                                                    AddressRange target_region) {
  return dsl::pipe([stage = mtkx_barrier_saturating_filter(source_region.to_c(),
                                                           target_region.to_c())](Slot slot)
                       -> std::optional<Slot> { return detail::run_stage(stage, slot); });
}

[[nodiscard]] inline auto barrier_record(std::vector<Slot>& buffer) {
  return dsl::pipe([&buffer](std::optional<Slot> slot) {
    if (slot.has_value()) {
      buffer.push_back(*slot);
    }
  });
}

[[nodiscard]] inline auto barrier_discard() {
  return dsl::pipe([](std::optional<Slot>) {});
}

/** RAII owner of an mtkx_slot_buffer. */
class SlotBuffer {
 public:
  SlotBuffer() noexcept { mtkx_slot_buffer_init(&b_); }
  SlotBuffer(const SlotBuffer&) = delete;
  SlotBuffer& operator=(const SlotBuffer&) = delete;
  ~SlotBuffer() { mtkx_slot_buffer_destroy(&b_); }

  [[nodiscard]] std::size_t size() const noexcept { return b_.size; }
  [[nodiscard]] bool empty() const noexcept { return b_.size == 0; }
  [[nodiscard]] const Slot& operator[](std::size_t i) const noexcept { return b_.data[i]; }
  [[nodiscard]] const Slot* begin() const noexcept { return b_.data; }
  [[nodiscard]] const Slot* end() const noexcept { return b_.data + b_.size; }
  [[nodiscard]] std::size_t dropped() const noexcept { return b_.dropped; }
  void clear() noexcept { mtkx_slot_buffer_clear(&b_); }
  [[nodiscard]] mtkx_slot_buffer* c_buffer() noexcept { return &b_; }

 private:
  mtkx_slot_buffer b_;
};

/** Records @p slot when it crosses from @p source_region into @p target_region. */
inline void run_write_barrier(BarrierDSL& barrier, Slot slot, AddressRange source_region,
                              AddressRange target_region, std::vector<Slot>& mod_buffer) {
  barrier.wrap(slot) | barrier_saturating_filter(source_region, target_region) |
      barrier_record(mod_buffer);
}

/** C-buffer variant: recording happens entirely in C. */
inline bool run_write_barrier(Slot slot, AddressRange source_region, AddressRange target_region,
                              SlotBuffer& mod_buffer) {
  return mtkx_run_write_barrier(slot, source_region.to_c(), target_region.to_c(),
                                mod_buffer.c_buffer());
}

}  // namespace MemTKX
