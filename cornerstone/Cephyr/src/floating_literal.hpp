#ifndef CEPHYR_FLOATING_LITERAL_HPP
#define CEPHYR_FLOATING_LITERAL_HPP

#include <bit>
#include <cfenv>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace cephyr::detail {

enum class FloatingLiteralStatus {
    ok,
    malformed,
    unsupported_long_double,
    overflow,
    too_long,
};

struct FloatingLiteral {
    FloatingLiteralStatus status = FloatingLiteralStatus::malformed;
    unsigned width = 0;
    std::uint64_t bits = 0;
    // Upper payload word: sign/exponent for x87; all 64 bits for binary128.
    std::uint64_t high_word = 0;

    explicit operator bool() const { return status == FloatingLiteralStatus::ok; }
};

namespace floating_literal_detail {

constexpr int digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 16;
}

// Round a binary significand right with nearest-even; all callers have
// at most 53 significant bits before a shift of at least 64.
constexpr std::uint64_t round_right(std::uint64_t value, unsigned shift) {
    if (!shift)
        return value;
    if (shift >= 64)
        return 0;
    const auto half = UINT64_C(1) << (shift - 1);
    const auto lo = value & ((half << 1) - 1);
    const auto hi = value >> shift;
    return hi + (lo > half || (lo == half && (hi & 1)));
}

extern "C" int cephyr_literal_bits(const char *, unsigned width, std::uint64_t *low,
                                  std::uint64_t *high);

} // namespace floating_literal_detail

// C11 6.4.4.2: input is one unsigned token, not a signed expression.
// Convert once, directly to the nominal format, independently of locale.
// This helper does
// not make target ABI decisions: a producer must separately gate FP lowering.
inline FloatingLiteral parse_floating_literal(std::string_view token, bool sysv_x87 = false,
                                             bool binary128 = false) {
    using floating_literal_detail::digit;
    constexpr std::int64_t token_limit = 1024 * 1024;
    if (token.empty())
        return {};
    if (token.size() > token_limit)
        return {FloatingLiteralStatus::too_long};
    bool single = false, extended = false;
    switch (token.back()) {
    case 'f': case 'F':
        single = true;
        token.remove_suffix(1);
        break;
    case 'l': case 'L':
        extended = true;
        token.remove_suffix(1);
        break;
    default:
        break;
    }
    const auto spelling = token;
    const bool hex = token.starts_with("0x") || token.starts_with("0X");
    if (hex)
        token.remove_prefix(2); // Validate the significand after the prefix.

    std::size_t i = 0;
    std::size_t count = 0;
    bool point = false;
    for (; i < token.size(); ++i) {
        const char c = token[i];
        if (c == '.' && !point) {
            point = true;
            continue;
        }
        const int d = digit(c);
        if (d >= (hex ? 16 : 10))
            break;
        ++count;
    }
    if (!count)
        return {};

    const bool has_exponent = i < token.size() &&
        (hex ? token[i] == 'p' || token[i] == 'P' : token[i] == 'e' || token[i] == 'E');
    if (has_exponent) {
        ++i;
        if (i < token.size() && (token[i] == '-' || token[i] == '+'))
            ++i;
        const auto begin = i;
        while (i < token.size() && digit(token[i]) < 10) ++i;
        if (i == begin) return {};
    }
    if (i != token.size() || (hex ? !has_exponent : !point && !has_exponent))
        return {};
    if (extended && !sysv_x87 && !binary128)
        return {FloatingLiteralStatus::unsupported_long_double};

    const unsigned width = extended ? (binary128 ? 128 : 80) : single ? 32 : 64;
    FloatingLiteral result{FloatingLiteralStatus::ok, width};
    if (!floating_literal_detail::cephyr_literal_bits(std::string(spelling).c_str(), width,
                                                     &result.bits, &result.high_word))
        result.status = FloatingLiteralStatus::overflow;
    return result;
}

// Apply a static initializer's f32/f64 assignment conversion after parsing
// to the token's nominal type. In particular, unsuffixed input first rounds
// to f64. Accept only finite bits from the literal/initializer path.
inline FloatingLiteral convert_floating_literal(FloatingLiteral value, unsigned width) {
    using floating_literal_detail::round_right;
    if (!value)
        return value;
    if ((width != 32 && width != 64) || (value.width != 32 && value.width != 64))
        return {};
    if (value.width == 32) {
        const auto exponent = (value.bits >> 23) & 0xff;
        if (value.bits > UINT32_MAX || exponent == 0xff)
            return {};
        if (width == 32)
            return value;
        const auto sign = (value.bits >> 31) << 63;
        const auto fraction = value.bits & UINT64_C(0x7fffff);
        if (exponent)
            return {FloatingLiteralStatus::ok, 64, sign | ((exponent + 896) << 52) | (fraction << 29)};
        if (!fraction)
            return {FloatingLiteralStatus::ok, 64, sign};
        const auto top = std::bit_width(fraction) - 1;
        const auto mantissa = (fraction ^ (UINT64_C(1) << top)) << (52 - top);
        return {FloatingLiteralStatus::ok, 64, sign | (std::uint64_t(874 + top) << 52) | mantissa};
    }

    const auto encoded_exponent = (value.bits >> 52) & 0x7ff;
    if (encoded_exponent == 0x7ff)
        return {};
    if (width == 64)
        return value;
    const auto sign = (value.bits >> 63) << 31;
    auto exponent = static_cast<int>(encoded_exponent) - 1023;
    if (exponent > 127)
        return {FloatingLiteralStatus::overflow, 32};
    if (exponent < -150)
        return {FloatingLiteralStatus::ok, 32, sign};
    const auto mantissa = (value.bits & UINT64_C(0xfffffffffffff)) | (UINT64_C(1) << 52);
    auto rounded = round_right(mantissa, 29 + (exponent < -126 ? -126 - exponent : 0));
    if (exponent < -126)
        return {FloatingLiteralStatus::ok, 32, sign | rounded};
    if (rounded == (UINT64_C(1) << 24)) {
        rounded >>= 1;
        if (++exponent > 127)
            return {FloatingLiteralStatus::overflow, 32};
    }
    const auto exp_bits = std::uint64_t(exponent + 127) << 23;
    return {FloatingLiteralStatus::ok, 32, sign | exp_bits | (rounded & UINT64_C(0x7fffff))};
}

// The caller supplies the canonical i64 bit pattern (sign/zero extended
// from its C type). Convert without host FP arithmetic or runtime fenv.
inline FloatingLiteral integer_to_floating_literal(std::uint64_t value, bool is_signed, unsigned width) {
    if (width != 32 && width != 64 && width != 80 && width != 128)
        return {};
    if (!value)
        return {FloatingLiteralStatus::ok, width, 0};
    const bool negative = is_signed && (value >> 63);
    const auto magnitude = negative ? -value : value;
    if (width == 128) {
        const unsigned top = std::bit_width(magnitude) - 1;
        const auto fraction = static_cast<unsigned __int128>(magnitude ^ (UINT64_C(1) << top)) << (112 - top);
        return {FloatingLiteralStatus::ok, 128, std::uint64_t(fraction),
                (std::uint64_t(negative) << 63) | (std::uint64_t(top + 16383) << 48) |
                    std::uint64_t(fraction >> 64)};
    }
    if (width == 80) {
        const auto top = std::bit_width(magnitude) - 1;
        return {FloatingLiteralStatus::ok, 80, magnitude << (63 - top),
                static_cast<std::uint16_t>((negative ? 0x8000 : 0) | (top + 16383))};
    }
    const unsigned fraction = width == 32 ? 23 : 52;
    const unsigned bias = width == 32 ? 127 : 1023;
    unsigned top = std::bit_width(magnitude) - 1;
    auto rounded = top > fraction
        ? floating_literal_detail::round_right(magnitude, top - fraction)
        : magnitude << (fraction - top);
    if (rounded == (UINT64_C(2) << fraction)) {
        rounded >>= 1;
        ++top;
    }
    const auto sign = std::uint64_t(negative) << (width - 1);
    const auto exp_bits = std::uint64_t(top + bias) << fraction;
    return {FloatingLiteralStatus::ok, width,
            sign | exp_bits | (rounded & ((UINT64_C(1) << fraction) - 1))};
}

// C11 6.3.1.2 and 6.3.1.4: _Bool compares with zero; other integer types
// discard the fractional part and require a representable truncated result.
inline std::optional<std::uint64_t> floating_literal_to_integer(
    FloatingLiteral value, unsigned width, bool is_signed) {
    if (value && value.width == 128 && width && width <= 64) {
        const auto high = value.high_word;
        if (width == 1 && !is_signed)
            return value.bits || (high << 1);
        const int exp = int((high >> 48) & 0x7fff) - 16383;
        if (exp < 0) return 0;
        if (exp >= 64) return std::nullopt;
        const auto significand = (static_cast<unsigned __int128>((high & UINT64_C(0xffffffffffff)) |
                                 (UINT64_C(1) << 48)) << 64) | value.bits;
        const auto magnitude = std::uint64_t(significand >> (112 - exp));
        const bool negative = high >> 63;
        const auto limit = is_signed ? (UINT64_C(1) << (width - 1)) - !negative :
            width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
        if (magnitude > limit || (negative && !is_signed)) return std::nullopt;
        return negative ? -magnitude : magnitude;
    }
    if (!value || !width || width > 64 ||
        (value.width != 32 && value.width != 64 && value.width != 80) ||
        (value.width == 32 && value.bits > UINT32_MAX))
        return std::nullopt;
    if (value.width == 80) {
        const auto biased = value.high_word & 0x7fff;
        if (width == 1 && !is_signed)
            return value.bits != 0;
        const int exponent = biased - 16383;
        if (exponent < 0)
            return 0;
        if (exponent >= 64)
            return std::nullopt;
        const bool negative = value.high_word & 0x8000;
        const auto magnitude = value.bits >> (63 - exponent);
        const auto limit = is_signed
            ? (UINT64_C(1) << (width - 1)) - !negative
            : width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
        if (magnitude > limit || (negative && !is_signed))
            return std::nullopt;
        return negative ? -magnitude : magnitude;
    }
    const unsigned fraction_bits = value.width == 32 ? 23 : 52;
    const unsigned biased_max = value.width == 32 ? 0xff : 0x7ff;
    const auto biased = (value.bits >> fraction_bits) & biased_max;
    const auto fraction = value.bits & ((UINT64_C(1) << fraction_bits) - 1);
    const bool negative = value.bits >> (value.width - 1);
    if (width == 1 && !is_signed)
        return (biased | fraction) != 0;
    const int exponent = static_cast<int>(biased) - static_cast<int>(biased_max / 2);
    if (exponent < 0)
        return 0; // Includes the valid unsigned casts of values in (-1, 0).
    if (biased == biased_max || exponent >= 64)
        return std::nullopt;
    auto magnitude = (UINT64_C(1) << fraction_bits) | fraction;
    if (static_cast<unsigned>(exponent) < fraction_bits)
        magnitude >>= fraction_bits - exponent;
    else
        magnitude <<= exponent - fraction_bits;
    const auto limit = is_signed
        ? (UINT64_C(1) << (width - 1)) - !negative
        : width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
    if (magnitude > limit || (negative && !is_signed))
        return std::nullopt;
    return negative ? -magnitude : magnitude;
}

} // namespace cephyr::detail

#endif
