#pragma once
/**
 * @file memtkx/core/types.hpp
 * @brief C++ address vocabulary (wraps memtkx/core/types.h).
 */

#include <cstddef>
#include <cstdint>

#include "memtkx/core/types.h"
#include "memtkx/detail/interop.hpp"

namespace MemTKX {

using Address = std::uintptr_t;
using ObjectReference = std::uintptr_t;
using Word = std::uintptr_t;
using Offset = std::ptrdiff_t;
using Byte = std::byte;

static_assert(sizeof(Address) == sizeof(mtkx_address), "Address must match mtkx_address");

inline constexpr std::size_t kWordBytes = sizeof(Word);

/** Half-open `[start, end)`; converts to and from `mtkx_address_range`. */
struct AddressRange {
  Address start{0};
  Address end{0};

  [[nodiscard]] constexpr bool empty() const noexcept { return start >= end; }
  [[nodiscard]] constexpr std::size_t size() const noexcept {
    return start >= end ? 0 : static_cast<std::size_t>(end - start);
  }
  [[nodiscard]] constexpr bool contains(Address addr) const noexcept {
    return addr >= start && addr < end;
  }
  [[nodiscard]] constexpr bool contains(AddressRange other) const noexcept {
    return other.start >= start && other.end <= end;
  }
  [[nodiscard]] constexpr bool overlaps(AddressRange other) const noexcept {
    return start < other.end && other.start < end;
  }
  [[nodiscard]] constexpr Address clamp(Address addr) const noexcept {
    if (addr < start) {
      return start;
    }
    if (addr >= end) {
      return end;
    }
    return addr;
  }
  friend constexpr bool operator==(AddressRange lhs, AddressRange rhs) noexcept {
    return lhs.start == rhs.start && lhs.end == rhs.end;
  }
  friend constexpr bool operator!=(AddressRange lhs, AddressRange rhs) noexcept {
    return !(lhs == rhs);
  }

  [[nodiscard]] constexpr mtkx_address_range to_c() const noexcept { return {start, end}; }
  [[nodiscard]] static constexpr AddressRange from_c(mtkx_address_range r) noexcept {
    return {r.start, r.end};
  }
};

[[nodiscard]] constexpr bool is_power_of_two(std::size_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] constexpr Address align_up(Address value, std::size_t alignment) noexcept {
  if (alignment <= 1) {
    return value;
  }
  const Address mask = static_cast<Address>(alignment - 1);
  return (value + mask) & ~mask;
}

[[nodiscard]] constexpr Address align_down(Address value, std::size_t alignment) noexcept {
  if (alignment <= 1) {
    return value;
  }
  const Address mask = static_cast<Address>(alignment - 1);
  return value & ~mask;
}

template <typename T>
[[nodiscard]] constexpr std::size_t object_alignment() noexcept {
  return alignof(T);
}

/** Same rule as mtkx_can_allocate(). */
[[nodiscard]] constexpr bool can_allocate(Address start, Address limit, std::size_t bytes,
                                          std::size_t alignment) noexcept {
  if (start >= limit) {
    return false;
  }
  const Address aligned = align_up(start, alignment);
  return aligned >= start && aligned <= limit &&
         static_cast<std::size_t>(limit - aligned) >= bytes;
}

template <typename T>
[[nodiscard]] inline Address as_address(T* ptr) noexcept {
  return reinterpret_cast<Address>(ptr);
}

template <typename T>
[[nodiscard]] inline T* from_address(Address addr) noexcept {
  return reinterpret_cast<T*>(addr);
}

}  // namespace MemTKX
