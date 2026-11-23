#ifndef CEPHYR_FLOATING_ARITHMETIC_HPP
#define CEPHYR_FLOATING_ARITHMETIC_HPP

#include "floating_literal.hpp"

#include <algorithm>

namespace cephyr::detail {

enum class FloatingOperation { add, subtract, multiply, divide };

namespace floating_arithmetic_detail {

using Wide = unsigned __int128;

struct Format {
    unsigned width;
    unsigned fraction() const { return width == 80 ? 63 : width == 32 ? 23 : 52; }
    int bias() const { return width == 80 ? 16383 : width == 32 ? 127 : 1023; }
    std::uint64_t hidden_bit() const { return UINT64_C(1) << fraction(); }
    std::uint64_t sign_bit() const { return UINT64_C(1) << (width == 80 ? 15 : width - 1); }
};

struct Number {
    bool negative;
    std::uint64_t significand;
    int power; // value = (-1)^negative * significand * 2^power
};

inline Number unpack(Format format, FloatingLiteral bits) {
    const bool x87 = format.width == 80;
    const auto head = x87 ? bits.high_word : bits.bits;
    const auto biased = static_cast<int>((head & (format.sign_bit() - 1)) >>
                                         (x87 ? 0 : format.fraction()));
    Number value{(head & format.sign_bit()) != 0, bits.bits & (format.hidden_bit() - 1),
                 std::max(biased, 1) - format.bias() - static_cast<int>(format.fraction())};
    if (biased) {
        value.significand |= format.hidden_bit();
    } else if (value.significand) {
        // Give subnormal inputs the same leading-bit position as normals.
        const unsigned shift = format.fraction() + 1 - std::bit_width(value.significand);
        value.significand <<= shift;
        value.power -= static_cast<int>(shift);
    }
    return value;
}

inline Wide shift_jam(Wide bits, unsigned shift) {
    if (!shift)
        return bits;
    if (shift >= 128)
        return bits != 0;
    const auto high = bits >> shift;
    return high | ((high << shift) != bits);
}

inline Wide round_nearest(Wide bits, int shift) {
    if (shift <= 0)
        return bits << -shift;
    if (shift > 128)
        return 0;
    const Wide half = Wide{1} << (shift - 1);
    if (shift == 128)
        return bits > half;
    const Wide tail = bits & ((half << 1) - 1);
    const Wide high = bits >> shift;
    return high + (tail > half || (tail == half && (high & 1)));
}

inline FloatingLiteral pack(Format format, bool negative, Wide significand, int power) {
    const auto sign = negative ? format.sign_bit() : 0;
    const bool x87 = format.width == 80;
    if (!significand)
        return {FloatingLiteralStatus::ok, format.width, x87 ? 0 : sign,
                static_cast<std::uint16_t>(x87 ? sign : 0)};
    const auto high = static_cast<std::uint64_t>(significand >> 64);
    const int top = high ? 63 + std::bit_width(high)
                         : std::bit_width(static_cast<std::uint64_t>(significand)) - 1;
    int exponent = top + power;
    const int minimum = 1 - format.bias();
    const int fraction_bits = static_cast<int>(format.fraction());
    if (exponent < minimum) {
        // Round once at the subnormal precision; carry may produce the
        // smallest normal. A rounded zero keeps its sign.
        const auto rounded = round_nearest(significand, minimum - fraction_bits - power);
        if (x87)
            return {FloatingLiteralStatus::ok, 80, static_cast<std::uint64_t>(rounded),
                    static_cast<std::uint16_t>(sign | (rounded >> 63))};
        return {FloatingLiteralStatus::ok, format.width, sign | static_cast<std::uint64_t>(rounded)};
    }
    auto rounded = round_nearest(significand, top - fraction_bits);
    if (rounded == (Wide(format.hidden_bit()) << 1)) {
        rounded >>= 1;
        ++exponent;
    }
    if (exponent > format.bias())
        return {FloatingLiteralStatus::overflow, format.width};
    if (x87)
        return {FloatingLiteralStatus::ok, 80, static_cast<std::uint64_t>(rounded),
                static_cast<std::uint16_t>(sign | (exponent + format.bias()))};
    const auto biased = std::uint64_t(exponent + format.bias()) << format.fraction();
    return {FloatingLiteralStatus::ok, format.width,
            sign | biased | (static_cast<std::uint64_t>(rounded) & (format.hidden_bit() - 1))};
}

inline bool valid_finite(Format format, FloatingLiteral value) {
    if (format.width == 80) {
        const auto biased = value.high_word & 0x7fff;
        return value && value.width == 80 && biased != 0x7fff &&
               (value.bits >> 63) == (biased != 0);
    }
    return value && value.width == format.width &&
           (format.width == 64 || (format.width == 32 && value.bits <= UINT32_MAX)) &&
           ((value.bits & (format.sign_bit() - 1)) >> format.fraction()) <
               static_cast<unsigned>(2 * format.bias() + 1);
}

} // namespace floating_arithmetic_detail

FloatingLiteral convert_quad_format(FloatingLiteral value, unsigned width);
FloatingLiteral fold_quad(FloatingOperation op, FloatingLiteral lhs, FloatingLiteral rhs);

inline FloatingLiteral convert_x87_literal(FloatingLiteral value, unsigned width) {
    if (width == 128 || value.width == 128)
        return convert_quad_format(value, width);
    using namespace floating_arithmetic_detail;
    if (!valid_finite(Format{value.width}, value) ||
        (width != 32 && width != 64 && width != 80))
        return {};
    if (width == value.width)
        return value;
    const auto number = unpack(Format{value.width}, value);
    return pack(Format{width}, number.negative, number.significand, number.power);
}

// C11 6.6p4–5: finite arithmetic at the target's nominal precision. Integer
// significands make the fold independent of the compiler host's FP environment.
// An unsuccessful result with status "malformed" includes division by zero.
inline FloatingLiteral floating_arithmetic(
    FloatingOperation operation, FloatingLiteral lhs, FloatingLiteral rhs) {
    if (lhs.width == 128 && rhs.width == 128)
        return fold_quad(operation, lhs, rhs);
    using namespace floating_arithmetic_detail;
    const Format format{lhs.width};
    if (!valid_finite(format, lhs) || !valid_finite(format, rhs))
        return {};
    if (operation == FloatingOperation::subtract) {
        if (format.width == 80)
            rhs.high_word ^= format.sign_bit();
        else
            rhs.bits ^= format.sign_bit();
    }
    auto a = unpack(format, lhs);
    auto b = unpack(format, rhs);

    switch (operation) {
    case FloatingOperation::add:
    case FloatingOperation::subtract: {
        if (!a.significand && !b.significand)
            return pack(format, a.negative && b.negative, 0, 0);
        if (!a.significand)
            return rhs;
        if (!b.significand)
            return lhs;
        if (a.power < b.power || (a.power == b.power && a.significand < b.significand))
            std::swap(a, b);
        // Guard, round, and sticky bits. Cancellation that shifts the result
        // left by more than one bit requires an exponent distance at most one,
        // where this alignment is still exact.
        constexpr int extra = 3;
        const auto left = Wide(a.significand) << extra;
        const auto right = shift_jam(Wide(b.significand) << extra, a.power - b.power);
        const auto result = a.negative == b.negative ? left + right : left - right;
        return pack(format, result && a.negative, result, a.power - extra);
    }
    case FloatingOperation::multiply:
        return pack(format, a.negative != b.negative,
                    Wide(a.significand) * b.significand, a.power + b.power);
    case FloatingOperation::divide: {
        if (!b.significand)
            return {};
        // Produce a quotient with at least three bits beyond the rounded
        // precision (their ratio is in [1/2, 2)), plus a sticky remainder.
        const int shift = static_cast<int>(format.fraction()) + 4;
        const int in_range = std::min(shift, 64);
        const auto numerator = Wide(a.significand) << in_range;
        auto quotient = numerator / b.significand;
        auto remainder = numerator % b.significand;
        for (int i = in_range; i < shift; ++i) {
            quotient <<= 1;
            remainder <<= 1;
            if (remainder >= b.significand) {
                quotient |= 1;
                remainder -= b.significand;
            }
        }
        return pack(format, a.negative != b.negative, quotient | (remainder != 0),
                    a.power - b.power - shift);
    }
    }
    return {};
}

// The evaluator supplies finite, same-format operands. Both zero signs compare
// equal; otherwise the magnitude encoding is monotonic.
inline int compare_finite_floating(FloatingLiteral lhs, FloatingLiteral rhs) {
    if (lhs.width == 80 || lhs.width == 128) {
        const auto sign = UINT64_C(1) << (lhs.width == 128 ? 63 : 15);
        const auto a = ((floating_arithmetic_detail::Wide(lhs.high_word & (sign - 1))) << 64) | lhs.bits;
        const auto b = ((floating_arithmetic_detail::Wide(rhs.high_word & (sign - 1))) << 64) | rhs.bits;
        if (!(a | b))
            return 0;
        const bool lhs_negative = lhs.high_word & sign;
        const bool rhs_negative = rhs.high_word & sign;
        if (lhs_negative != rhs_negative)
            return lhs_negative ? -1 : 1;
        return a == b ? 0 : ((a < b) != lhs_negative) ? -1 : 1;
    }
    const auto sign = floating_arithmetic_detail::Format{lhs.width}.sign_bit();
    if (((lhs.bits | rhs.bits) & (sign - 1)) == 0 || lhs.bits == rhs.bits)
        return 0;
    const bool lhs_negative = (lhs.bits & sign) != 0;
    if ((lhs.bits ^ rhs.bits) & sign)
        return lhs_negative ? -1 : 1;
    return ((lhs.bits < rhs.bits) != lhs_negative) ? -1 : 1;
}

} // namespace cephyr::detail

#endif
