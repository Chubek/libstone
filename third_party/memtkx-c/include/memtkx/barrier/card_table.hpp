#pragma once
/**
 * @file memtkx/barrier/card_table.hpp
 * @brief `CardTable` and `CardTableBarrier` (wrap memtkx/barrier/card_table.h).
 */

#include <cstddef>
#include <new>
#include <utility>

#include "memtkx/barrier/card_table.h"
#include "memtkx/core/types.hpp"

namespace MemTKX {

/** One byte per card; move-only. */
class CardTable {
 public:
  CardTable() noexcept = default;
  CardTable(Address base, std::size_t region_bytes, std::size_t card_bytes = 512) {
    detail::throw_if_nomem(mtkx_card_table_init(&t_, base, region_bytes, card_bytes));
  }
  CardTable(const CardTable&) = delete;
  CardTable& operator=(const CardTable&) = delete;
  CardTable(CardTable&& other) noexcept : t_(std::exchange(other.t_, mtkx_card_table{})) {}
  CardTable& operator=(CardTable&& other) noexcept {
    if (this != &other) {
      mtkx_card_table_destroy(&t_);
      t_ = std::exchange(other.t_, mtkx_card_table{});
    }
    return *this;
  }
  ~CardTable() { mtkx_card_table_destroy(&t_); }

  [[nodiscard]] std::size_t card_index(Address address) const noexcept {
    return mtkx_card_table_card_index(&t_, address);
  }
  void mark(Address address) noexcept { mtkx_card_table_mark(&t_, address); }
  [[nodiscard]] bool is_marked(Address address) const noexcept {
    return mtkx_card_table_is_marked(&t_, address);
  }
  void clear(Address address) noexcept { mtkx_card_table_clear(&t_, address); }
  void clear_all() noexcept { mtkx_card_table_clear_all(&t_); }
  [[nodiscard]] std::size_t marked_cards() const noexcept {
    return mtkx_card_table_marked_cards(&t_);
  }
  [[nodiscard]] std::size_t card_count() const noexcept { return mtkx_card_table_card_count(&t_); }
  [[nodiscard]] std::size_t card_bytes() const noexcept { return mtkx_card_table_card_bytes(&t_); }
  [[nodiscard]] Address base() const noexcept { return mtkx_card_table_base(&t_); }

  [[nodiscard]] mtkx_card_table* c_table() noexcept { return &t_; }

 private:
  mtkx_card_table t_{0, 0, MTKX_DEFAULT_CARD_BYTES, nullptr, 0};
};

/** Marks the card of each written slot; does not perform the store. */
class CardTableBarrier {
 public:
  explicit CardTableBarrier(CardTable& table) noexcept
      : b_(mtkx_card_table_barrier_make(table.c_table())) {}

  void operator()(Address* slot_address, Address value) {
    mtkx_card_table_barrier_apply(&b_, slot_address, value);
  }

 private:
  mtkx_card_table_barrier b_;
};

}  // namespace MemTKX
