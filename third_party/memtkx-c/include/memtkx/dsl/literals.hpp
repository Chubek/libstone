#pragma once
/**
 * @file memtkx/dsl/literals.hpp
 * @brief Literal-suffix tables (`literal_set(lit<"_km">(f), ...)`), parsed
 *        by mtkx_literal_set_parse_n().
 */

#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "memtkx/dsl/core.hpp"
#include "memtkx/dsl/literals.h"

namespace MemTKX::dsl {

/** One suffix and its handler `long double(long double)`. */
template <typename F>
struct LiteralEntry {
  std::string_view suffix;
  F handler;
  constexpr LiteralEntry(std::string_view s, F f) : suffix(s), handler(std::move(f)) {}
};

/** Entry with a runtime suffix (a string literal or other long-lived text). */
template <typename F>
constexpr auto lit(std::string_view suffix, F&& f) {
  return LiteralEntry<std::decay_t<F>>{suffix, std::forward<F>(f)};
}

#if MEMTKX_CPP20
/** `lit<"_km">(f)` */
template <FixedString Suffix, typename F>
constexpr auto lit(F&& f) {
  return LiteralEntry<std::decay_t<F>>{Suffix.view(), std::forward<F>(f)};
}
#endif

/** An ordered suffix table. */
template <typename... Entries>
struct LiteralSet {
  std::tuple<Entries...> entries;
  explicit constexpr LiteralSet(Entries... es) : entries(std::move(es)...) {}

  /**
   * @brief Parses "3.5_km". Throws std::runtime_error when no suffix
   *        matches, std::invalid_argument / std::out_of_range for a bad
   *        number (as std::stold did).
   */
  long double parse(std::string_view input) const {
    constexpr std::size_t n = sizeof...(Entries);
    mtkx_literal_entry c_entries[n == 0 ? 1 : n];
    std::string suffixes[n == 0 ? 1 : n];
    std::size_t i = 0;
    std::apply(
        [&](const auto&... e) {
          (
              [&] {
                suffixes[i] = std::string(e.suffix);
                c_entries[i] = mtkx_lit(suffixes[i].c_str(), &trampoline<std::decay_t<decltype(e)>>,
                                        const_cast<void*>(static_cast<const void*>(&e)));
                ++i;
              }(),
              ...);
        },
        entries);
    const mtkx_literal_set set = mtkx_literal_set_make(c_entries, n);
    long double out = 0.0L;
    const mtkx_status status = mtkx_literal_set_parse_n(&set, input.data(), input.size(), &out);
    detail::rethrow_pending();
    switch (status) {
      case MTKX_OK:
        return out;
      case MTKX_ERR_NOT_FOUND:
        throw std::runtime_error("dsl::LiteralSet::parse: no suffix matched for '" +
                                 std::string(input) + "'");
      case MTKX_ERR_RANGE:
        throw std::out_of_range("dsl::LiteralSet::parse: value out of range");
      case MTKX_ERR_NOMEM:
        throw std::bad_alloc();
      default:
        throw std::invalid_argument("dsl::LiteralSet::parse: invalid number in '" +
                                    std::string(input) + "'");
    }
  }

 private:
  template <typename E>
  static long double trampoline(long double v, void* ctx) {
    if (detail::has_pending_exception()) {
      return 0.0L;
    }
    try {
      return static_cast<long double>(static_cast<const E*>(ctx)->handler(v));
    } catch (...) {
      detail::park_current_exception();
      return 0.0L;
    }
  }
};

template <typename... Entries>
constexpr auto literal_set(Entries&&... es) {
  return LiteralSet<std::decay_t<Entries>...>{std::forward<Entries>(es)...};
}

/** Feature: `parse_literal("5.0_km")` using `Derived::literals`. */
struct CustomLiterals {
  template <typename Derived>
  struct Mixin {
    static long double parse_literal(std::string_view input) {
      return Derived::literals.parse(input);
    }
  };
};

}  // namespace MemTKX::dsl
