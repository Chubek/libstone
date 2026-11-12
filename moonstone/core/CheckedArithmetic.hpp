#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace moonstone {
inline namespace v1 {
namespace arithmetic {

inline std::optional<std::int64_t> add(std::int64_t a, std::int64_t b) noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if ((b > 0 && a > max - b) || (b < 0 && a < min - b))
        return std::nullopt;
    return a + b;
}

inline std::optional<std::int64_t> sub(std::int64_t a, std::int64_t b) noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if ((b < 0 && a > max + b) || (b > 0 && a < min + b))
        return std::nullopt;
    return a - b;
}

inline std::optional<std::int64_t> mul(std::int64_t a, std::int64_t b) noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if (a == 0 || b == 0)
        return 0;
    if (a > 0) {
        if ((b > 0 && a > max / b) || (b < 0 && b < min / a))
            return std::nullopt;
    } else {
        if ((b > 0 && a < min / b) || (b < 0 && a < max / b))
            return std::nullopt;
    }
    return a * b;
}

inline std::optional<std::int64_t> neg(std::int64_t a) noexcept { return sub(0, a); }

} // namespace arithmetic
} // namespace v1
} // namespace moonstone
