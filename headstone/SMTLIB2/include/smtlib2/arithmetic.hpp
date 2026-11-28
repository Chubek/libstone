#pragma once
#include "generated_firestone.hpp"
#include <stdexcept>
#include <string_view>
#include <utility>

namespace smtlib2 {
using Integer = firestone::bignum::BigInt;

namespace arithmetic_detail {
inline bool digits(std::string_view text) noexcept {
    if (text.empty()) return false;
    for (char c : text)
        if (c < '0' || c > '9') return false;
    return true;
}
}

// Parse a nonnegative SMT-LIB numeral token. Negative values are terms, not tokens.
inline Integer parse_numeral(std::string_view text) {
    if (!arithmetic_detail::digits(text) || (text.size() > 1 && text.front() == '0'))
        throw std::invalid_argument("invalid SMT-LIB numeral");
    return Integer(text);
}

// SMT-LIB Ints: a = b*q + r, 0 <= r < abs(b), for b != 0.
// No particular value is imposed by the theory at b == 0. This concrete
// helper throws std::domain_error rather than choosing an interpretation.
inline std::pair<Integer, Integer> euclidean_divmod(const Integer& a, const Integer& b) {
    if (!b.negative()) return divmod(a, b);
    auto qr = divmod(a, -b);
    qr.first = -qr.first;
    return qr;
}

// Concrete exact real values represented by a reduced fraction with a
// positive denominator. Zero is always represented as 0/1.
class Rational {
public:
    explicit Rational(Integer numerator = {}, Integer denominator = Integer(1))
        : numerator_(std::move(numerator)), denominator_(std::move(denominator)) {
        if (denominator_.is_zero()) throw std::domain_error("zero rational denominator");
        if (denominator_.negative()) {
            numerator_ = -numerator_;
            denominator_ = -denominator_;
        }
        auto divisor = gcd(numerator_, denominator_);
        numerator_ /= divisor;
        denominator_ /= divisor;
    }

    // Copying from an rvalue also leaves its denominator valid.
    Rational(const Rational&) = default;
    Rational& operator=(Rational other) noexcept {
        using std::swap;
        swap(numerator_, other.numerator_);
        swap(denominator_, other.denominator_);
        return *this;
    }

    const Integer& numerator() const noexcept { return numerator_; }
    const Integer& denominator() const noexcept { return denominator_; }
    Integer floor() const { return numerator_ / denominator_; }
    bool is_integer() const noexcept { return denominator_.bit_length() == 1; }

    Rational operator-() const { return Rational(-numerator_, denominator_); }
    friend Rational operator+(const Rational& a, const Rational& b) {
        return Rational(a.numerator_ * b.denominator_ + b.numerator_ * a.denominator_,
                        a.denominator_ * b.denominator_);
    }
    friend Rational operator-(const Rational& a, const Rational& b) { return a + (-b); }
    friend Rational operator*(const Rational& a, const Rational& b) {
        return Rational(a.numerator_ * b.numerator_, a.denominator_ * b.denominator_);
    }
    // Real division at zero is underspecified in SMT-LIB, not an error in the
    // language. The concrete helper does not select a value for that case.
    friend Rational operator/(const Rational& a, const Rational& b) {
        return Rational(a.numerator_ * b.denominator_, a.denominator_ * b.numerator_);
    }
    friend bool operator==(const Rational& a, const Rational& b) noexcept {
        return a.numerator_ == b.numerator_ && a.denominator_ == b.denominator_;
    }
    friend bool operator!=(const Rational& a, const Rational& b) noexcept { return !(a == b); }
    friend bool operator<(const Rational& a, const Rational& b) {
        return a.numerator_ * b.denominator_ < b.numerator_ * a.denominator_;
    }
    friend bool operator>(const Rational& a, const Rational& b) { return b < a; }
    friend bool operator<=(const Rational& a, const Rational& b) { return !(b < a); }
    friend bool operator>=(const Rational& a, const Rational& b) { return !(a < b); }

private:
    Integer numerator_, denominator_;
};

// Parse a nonnegative SMT-LIB decimal token, with no host floating point.
inline Rational parse_decimal(std::string_view text) {
    const auto dot = text.find('.');
    if (dot == std::string_view::npos) throw std::invalid_argument("invalid SMT-LIB decimal");
    auto fraction = text.substr(dot + 1);
    if (!arithmetic_detail::digits(fraction)) throw std::invalid_argument("invalid SMT-LIB decimal");
    auto integer = parse_numeral(text.substr(0, dot));
    while (!fraction.empty() && fraction.back() == '0') fraction.remove_suffix(1);
    if (fraction.empty()) return Rational(std::move(integer));
    auto scale = pow(Integer(10), fraction.size());
    auto numerator = integer * scale + Integer(fraction);
    return Rational(std::move(numerator), std::move(scale));
}
}
