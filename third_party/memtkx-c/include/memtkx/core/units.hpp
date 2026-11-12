#pragma once
/**
 * @file memtkx/core/units.hpp
 * @brief Binary memory literals and conversions (wraps memtkx/core/units.h).
 */

#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include "memtkx/MemTKXDSL.hpp"
#include "memtkx/core/units.h"

namespace MemTKX {

/** Runtime unit parsing ("1.5_MB"), via mtkx_units_parse_literal(). */
struct MemoryUnitsDSL : dsl::DSL<MemoryUnitsDSL> {
  /** @return the byte count; throws like the original (runtime_error / invalid_argument). */
  static long double parse_literal(std::string_view input) {
    const std::string text(input);
    long double out = 0.0L;
    switch (mtkx_units_parse_literal(text.c_str(), &out)) {
      case MTKX_OK:
        return out;
      case MTKX_ERR_NOT_FOUND:
        throw std::runtime_error("dsl::LiteralSet::parse: no suffix matched for '" + text + "'");
      case MTKX_ERR_RANGE:
        throw std::out_of_range("MemoryUnitsDSL::parse_literal: value out of range");
      case MTKX_ERR_NOMEM:
        throw std::bad_alloc();
      default:
        throw std::invalid_argument("MemoryUnitsDSL::parse_literal: invalid number in '" +
                                    text + "'");
    }
  }
};

inline namespace literals {

[[nodiscard]] constexpr std::size_t operator""_B(unsigned long long value) noexcept {
  return static_cast<std::size_t>(value);
}
[[nodiscard]] constexpr std::size_t operator""_KB(unsigned long long value) noexcept {
  return static_cast<std::size_t>(value * 1024ULL);
}
[[nodiscard]] constexpr std::size_t operator""_MB(unsigned long long value) noexcept {
  return static_cast<std::size_t>(value * 1024ULL * 1024ULL);
}
[[nodiscard]] constexpr std::size_t operator""_GB(unsigned long long value) noexcept {
  return static_cast<std::size_t>(value * 1024ULL * 1024ULL * 1024ULL);
}

}  // namespace literals

[[nodiscard]] constexpr std::size_t bytes_to_kib(std::size_t bytes) noexcept { return bytes / 1024; }
[[nodiscard]] constexpr std::size_t kib_to_bytes(std::size_t kib) noexcept { return kib * 1024; }
[[nodiscard]] constexpr std::size_t bytes_to_mib(std::size_t bytes) noexcept {
  return bytes / (1024 * 1024);
}
[[nodiscard]] constexpr std::size_t mib_to_bytes(std::size_t mib) noexcept {
  return mib * 1024 * 1024;
}

}  // namespace MemTKX
