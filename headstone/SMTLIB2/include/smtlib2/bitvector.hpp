#pragma once
#include "arithmetic.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace smtlib2 {

/// @implstring Concrete fixed-width values only; no term elaboration or solving.
/// Store the canonical unsigned residue and interpret it as two's complement
/// for signed operations. Same-sort operations reject different widths; C++
/// equality compares both width and value.
class BitVector {
public:
    // The bundled Integer supports left shifts through this bound.
    static constexpr std::size_t max_width = std::size_t{1} << 26;

    // Reduce arbitrary signed integers modulo 2^width. Zero width is invalid;
    // widths beyond the implementation limit throw std::length_error.
    explicit BitVector(std::size_t width, Integer value = {})
        : width_(checked_width(width)), value_(wrap(width_, std::move(value))) {}

    BitVector(const BitVector&) = default;
    // The moved-from Integer becomes zero, still a valid value of its width.
    BitVector(BitVector&&) noexcept = default;
    BitVector& operator=(BitVector other) noexcept {
        width_ = other.width_;
        value_ = std::move(other.value_);
        return *this;
    }

    std::size_t width() const noexcept { return width_; }
    const Integer& unsigned_value() const noexcept { return value_; }
    Integer signed_value() const { return negative() ? value_ - power_of_two(width_) : value_; }

    friend bool operator==(const BitVector& a, const BitVector& b) noexcept {
        return a.width_ == b.width_ && a.value_ == b.value_;
    }
    friend bool operator!=(const BitVector& a, const BitVector& b) noexcept { return !(a == b); }

    int unsigned_compare(const BitVector& rhs) const {
        require_same_width(rhs);
        return compare(value_, rhs.value_);
    }
    int signed_compare(const BitVector& rhs) const {
        require_same_width(rhs);
        if (negative() != rhs.negative()) return negative() ? -1 : 1;
        return compare(value_, rhs.value_);
    }

    BitVector operator-() const { return BitVector(width_, -value_); }
    BitVector operator~() const { return BitVector(width_, all_ones() - value_); }

    friend BitVector operator+(const BitVector& a, const BitVector& b) {
        a.require_same_width(b);
        return BitVector(a.width_, a.value_ + b.value_);
    }
    friend BitVector operator-(const BitVector& a, const BitVector& b) {
        a.require_same_width(b);
        return BitVector(a.width_, a.value_ - b.value_);
    }
    friend BitVector operator*(const BitVector& a, const BitVector& b) {
        a.require_same_width(b);
        return BitVector(a.width_, a.value_ * b.value_);
    }
    friend BitVector operator&(const BitVector& a, const BitVector& b) {
        return a.bitwise(b, [](bool x, bool y) { return x && y; });
    }
    friend BitVector operator|(const BitVector& a, const BitVector& b) {
        return a.bitwise(b, [](bool x, bool y) { return x || y; });
    }
    friend BitVector operator^(const BitVector& a, const BitVector& b) {
        return a.bitwise(b, [](bool x, bool y) { return x != y; });
    }

    // Shift distances are same-width *unsigned* bit vectors. Distances at
    // least width give all zero bits or, for arithmetic shift, all sign bits.
    BitVector shl(const BitVector& amount) const {
        const auto n = shift_amount(amount);
        return BitVector(width_, n == width_ ? Integer{} : value_ << n);
    }
    BitVector lshr(const BitVector& amount) const {
        const auto n = shift_amount(amount);
        return BitVector(width_, n == width_ ? Integer{} : value_ >> n);
    }
    BitVector ashr(const BitVector& amount) const {
        const auto n = shift_amount(amount);
        return BitVector(width_, n == width_ ? Integer(negative() ? -1 : 0) : signed_value() >> n);
    }

    // Indices are inclusive, with index zero denoting the least-significant bit.
    BitVector extract(std::size_t high, std::size_t low) const {
        if (high >= width_ || low > high)
            throw std::out_of_range("invalid bit-vector extract indices");
        return BitVector(high - low + 1, value_ >> low);
    }
    BitVector concat(const BitVector& low) const {
        if (low.width_ > max_width - width_)
            throw std::length_error("bit-vector concatenation exceeds width limit");
        return BitVector(width_ + low.width_, (value_ << low.width_) + low.value_);
    }

    // FixedSizeBitVectors defines these operations even at a zero divisor.
    BitVector udiv(const BitVector& rhs) const {
        require_same_width(rhs);
        return BitVector(width_, rhs.value_.is_zero() ? all_ones() : value_ / rhs.value_);
    }
    BitVector urem(const BitVector& rhs) const {
        require_same_width(rhs);
        return rhs.value_.is_zero() ? *this : BitVector(width_, value_ % rhs.value_);
    }
    BitVector sdiv(const BitVector& rhs) const {
        require_same_width(rhs);
        if (rhs.value_.is_zero()) return BitVector(width_, negative() ? 1 : -1);
        // Divide magnitudes to truncate toward zero, unlike Integer division
        // on signed inputs. Construction wraps signed minimum / -1.
        auto quotient = signed_value().abs() / rhs.signed_value().abs();
        return BitVector(width_, negative() != rhs.negative() ? -quotient : std::move(quotient));
    }
    BitVector srem(const BitVector& rhs) const {
        require_same_width(rhs);
        if (rhs.value_.is_zero()) return *this;
        auto remainder = signed_value().abs() % rhs.signed_value().abs();
        return BitVector(width_, negative() ? -remainder : std::move(remainder));
    }
    BitVector smod(const BitVector& rhs) const {
        auto result = srem(rhs);
        // Nonzero srem follows the dividend's sign; smod follows the divisor.
        if (!result.value_.is_zero() && !rhs.value_.is_zero() && negative() != rhs.negative())
            return result + rhs;
        return result;
    }

private:
    std::size_t width_;
    Integer value_;

    static std::size_t checked_width(std::size_t width) {
        if (!width) throw std::invalid_argument("bit-vector width must be positive");
        if (width > max_width) throw std::length_error("bit-vector width exceeds implementation limit");
        return width;
    }
    static Integer power_of_two(std::size_t n) { return Integer(1) << n; }
    static Integer wrap(std::size_t width, Integer value) {
        const bool negative = value.negative();
        if (negative) value = -value;
        // Truncate the magnitude without using BigInt's general division.
        if (value.bit_length() > width) value -= (value >> width) << width;
        if (negative && !value.is_zero()) return power_of_two(width) - value;
        return value;
    }
    bool negative() const noexcept { return value_.test_bit(width_ - 1); }
    Integer all_ones() const { return power_of_two(width_) - 1; }
    void require_same_width(const BitVector& rhs) const {
        if (width_ != rhs.width_) throw std::invalid_argument("bit-vector widths differ");
    }
    std::size_t shift_amount(const BitVector& amount) const {
        require_same_width(amount);
        if (amount.value_ >= Integer(static_cast<std::int64_t>(width_))) return width_;
        return static_cast<std::size_t>(amount.value_.to_int64());
    }

    template<class Op>
    BitVector bitwise(const BitVector& rhs, Op op) const {
        require_same_width(rhs);
        const auto length = std::max(value_.bit_length(), rhs.value_.bit_length());
        return BitVector(width_, combine_bits(rhs, op, 0, length));
    }
    template<class Op>
    Integer combine_bits(const BitVector& rhs, Op op, std::size_t start, std::size_t length) const {
        if (length <= 32) {
            std::uint32_t chunk = 0;
            for (std::size_t i = 0; i < length; ++i)
                if (op(value_.test_bit(start + i), rhs.value_.test_bit(start + i)))
                    chunk |= std::uint32_t{1} << i;
            return Integer(chunk);
        }
        // Merge chunks in a tree rather than repeatedly shifting a growing
        // Integer, which would copy a quadratic number of limbs.
        const auto half = length / 2;
        return (combine_bits(rhs, op, start + half, length - half) << half) +
               combine_bits(rhs, op, start, half);
    }
};
}
