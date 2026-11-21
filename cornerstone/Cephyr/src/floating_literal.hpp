#ifndef CEPHYR_FLOATING_LITERAL_HPP
#define CEPHYR_FLOATING_LITERAL_HPP

#include <bit>
#include <cfenv>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <locale.h>
#include <optional>
#include <string>
#include <string_view>

namespace cephyr::detail {

enum class FloatingLiteralStatus {
    ok,
    malformed,
    unsupported_long_double,
    unsupported_host,
    unsupported_x87_host,
    overflow,
    too_long,
};

struct FloatingLiteral {
    FloatingLiteralStatus status = FloatingLiteralStatus::malformed;
    unsigned width = 0;
    std::uint64_t bits = 0;
    // Only for width 80: bits holds the explicit significand, not padding.
    std::uint16_t x87_sign_exponent = 0;

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

template<class Real, class UInt, int Precision, int MaxExponent, UInt OneBits>
FloatingLiteral translate(std::string_view spelling, bool hex, bool below_one) {
    constexpr unsigned width = std::numeric_limits<UInt>::digits;
    if constexpr (sizeof(Real) != sizeof(UInt) ||
                  !std::numeric_limits<Real>::is_iec559 ||
                  std::numeric_limits<Real>::radix != 2 ||
                  std::numeric_limits<Real>::digits != Precision ||
                  std::numeric_limits<Real>::max_exponent != MaxExponent) {
        return {FloatingLiteralStatus::unsupported_host, width};
    } else {
        // Guard exotic host layouts before extracting a target bit pattern.
        if constexpr (std::bit_cast<UInt>(Real{1}) != OneBits) {
            return {FloatingLiteralStatus::unsupported_host, width};
        } else {
            Real value{};
            const auto end = spelling.data() + spelling.size();
            const auto parsed = std::from_chars(spelling.data(), end, value,
                hex ? std::chars_format::hex : std::chars_format::general);
            if (parsed.ptr != end)
                return {};
            if (parsed.ec == std::errc::result_out_of_range) {
                // from_chars reports both overflow and nonzero input that
                // rounds to zero as out_of_range. Only the latter is in range
                // for our translation policy.
                if (below_one)
                    return {FloatingLiteralStatus::ok, width, 0};
                return {FloatingLiteralStatus::overflow, width};
            }
            if (parsed.ec != std::errc{})
                return {};
            return {FloatingLiteralStatus::ok, width, std::bit_cast<UInt>(value)};
        }
    }
}

inline FloatingLiteral translate_x87_hex(std::string_view significand, std::int64_t top) {
    // The least significant subnormal bit is 2**-16445. Keep at most
    // 64 bits and round once, to nearest-even, including at that boundary.
    if (top > 16383)
        return {FloatingLiteralStatus::overflow, 80};
    if (top < -16446)
        return {FloatingLiteralStatus::ok, 80};
    const auto bottom = top >= -16382 ? top - 63 : -16445;
    auto position = top;
    bool started = false, halfway = false, sticky = false;
    std::uint64_t rounded = 0;
    for (const char c : significand) {
        if (c == '.')
            continue;
        const auto d = digit(c);
        for (int i = 3; i >= 0; --i) {
            const bool bit = (d >> i) & 1;
            if (!started && !bit)
                continue;
            started = true;
            if (position >= bottom)
                rounded = (rounded << 1) | bit;
            else if (position == bottom - 1)
                halfway = bit;
            else
                sticky |= bit;
            --position;
        }
    }
    if (position >= bottom)
        rounded <<= position - bottom + 1;
    if (halfway && (sticky || (rounded & 1))) {
        if (++rounded == 0) {
            rounded = UINT64_C(1) << 63;
            if (++top > 16383)
                return {FloatingLiteralStatus::overflow, 80};
        }
    }
    return {FloatingLiteralStatus::ok, 80, rounded,
            static_cast<std::uint16_t>(top >= -16382 ? top + 16383 : rounded >> 63)};
}

inline FloatingLiteral translate_x87_decimal([[maybe_unused]] std::string_view spelling) {
    // strtold_l translates directly to extended precision and handles the
    // nonzero subnormals rejected by some long double from_chars versions.
    // Use it only on a verified x87 host, in the C locale and nearest mode.
#if defined(__linux__)
    if constexpr (std::endian::native == std::endian::little &&
                  std::numeric_limits<long double>::is_iec559 &&
                  std::numeric_limits<long double>::digits == 64 &&
                  std::numeric_limits<long double>::max_exponent == 16384 &&
                  sizeof(long double) >= 10) {
        const auto payload = [](long double value) {
            FloatingLiteral result{FloatingLiteralStatus::ok, 80};
            std::memcpy(&result.bits, &value, 8);
            std::memcpy(&result.x87_sign_exponent,
                        reinterpret_cast<const unsigned char *>(&value) + 8, 2);
            return result;
        };
        const auto one = payload(1.0L);
        if (one.bits != (UINT64_C(1) << 63) || one.x87_sign_exponent != 0x3fff)
            return {FloatingLiteralStatus::unsupported_x87_host, 80};
        const std::string text(spelling);
        const auto locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
        if (!locale)
            return {FloatingLiteralStatus::unsupported_x87_host, 80};
        std::fenv_t saved;
        const bool held = std::feholdexcept(&saved) == 0;
        const bool nearest = held && std::fesetround(FE_TONEAREST) == 0;
        char *end = nullptr;
        FloatingLiteral result{FloatingLiteralStatus::unsupported_x87_host, 80};
        if (nearest) {
            result = payload(strtold_l(text.c_str(), &end, locale));
            if (end != text.c_str() + text.size())
                result = {};
            else if (result.x87_sign_exponent >= 0x7fff)
                result = {FloatingLiteralStatus::overflow, 80};
        }
        if (held)
            std::fesetenv(&saved);
        freelocale(locale);
        return result;
    }
#endif
    return {FloatingLiteralStatus::unsupported_x87_host, 80};
}

} // namespace floating_literal_detail

// C11 6.4.4.2: input is one unsigned token, not a signed expression.
// Convert once, directly to the nominal format, independently of locale.
// Implementations with a non-IEEE host format fail closed. This helper does
// not make target ABI decisions: a producer must separately gate FP lowering.
inline FloatingLiteral parse_floating_literal(std::string_view token, bool sysv_x87 = false) {
    using floating_literal_detail::digit;
    constexpr std::int64_t token_limit = 1024 * 1024;
    // Bigger than any exponent that can be canceled by the significand's
    // length (at most four binary exponent positions per hex digit).
    constexpr std::int64_t exponent_limit = token_limit * 4 + 4096;
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
    const bool hex = token.starts_with("0x") || token.starts_with("0X");
    if (hex)
        token.remove_prefix(2); // from_chars hex mode does not take 0x.

    std::size_t i = 0;
    std::int64_t count = 0, whole = 0, first_nonzero = -1;
    int first_digit = 0;
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
        if (d && first_nonzero < 0) {
            first_nonzero = count;
            first_digit = d;
        }
        ++count;
        if (!point)
            ++whole;
    }
    if (!count)
        return {};

    const auto significand = token.substr(0, i);
    std::int64_t exponent = 0;
    const bool has_exponent = i < token.size() &&
        (hex ? token[i] == 'p' || token[i] == 'P' : token[i] == 'e' || token[i] == 'E');
    if (has_exponent) {
        ++i;
        const bool negative = i < token.size() && token[i] == '-';
        if (i < token.size() && (negative || token[i] == '+'))
            ++i;
        const auto begin = i;
        for (; i < token.size() && digit(token[i]) < 10; ++i) {
            if (exponent < exponent_limit) {
                exponent = exponent * 10 + digit(token[i]);
                if (exponent > exponent_limit)
                    exponent = exponent_limit;
            }
        }
        if (i == begin)
            return {};
        if (negative)
            exponent = -exponent;
    }
    if (i != token.size() || (hex ? !has_exponent : !point && !has_exponent))
        return {};
    if (extended && !sysv_x87)
        return {FloatingLiteralStatus::unsupported_long_double};

    const unsigned width = extended ? 80 : single ? 32 : 64;
    if (first_nonzero < 0)
        return {FloatingLiteralStatus::ok, width, 0};

    auto leading_exponent = whole - first_nonzero - 1;
    if (hex)
        leading_exponent = leading_exponent * 4 +
            (first_digit >= 8 ? 3 : first_digit >= 4 ? 2 : first_digit >= 2 ? 1 : 0);
    if (extended)
        return hex ? floating_literal_detail::translate_x87_hex(significand, leading_exponent + exponent)
                   : floating_literal_detail::translate_x87_decimal(token);
    // Only needed to distinguish over/underflow for from_chars.
    const bool below_one = leading_exponent + exponent < 0;
    using floating_literal_detail::translate;
    if (single)
        return translate<float, std::uint32_t, 24, 128, UINT32_C(0x3f800000)>(
            token, hex, below_one);
    return translate<double, std::uint64_t, 53, 1024, UINT64_C(0x3ff0000000000000)>(
        token, hex, below_one);
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
    if (width != 32 && width != 64 && width != 80)
        return {};
    if (!value)
        return {FloatingLiteralStatus::ok, width, 0};
    const bool negative = is_signed && (value >> 63);
    const auto magnitude = negative ? -value : value;
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
    if (!value || !width || width > 64 ||
        (value.width != 32 && value.width != 64 && value.width != 80) ||
        (value.width == 32 && value.bits > UINT32_MAX))
        return std::nullopt;
    if (value.width == 80) {
        const auto biased = value.x87_sign_exponent & 0x7fff;
        if (width == 1 && !is_signed)
            return value.bits != 0;
        const int exponent = biased - 16383;
        if (exponent < 0)
            return 0;
        if (exponent >= 64)
            return std::nullopt;
        const bool negative = value.x87_sign_exponent & 0x8000;
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
