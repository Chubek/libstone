#ifdef NDEBUG
#undef NDEBUG
#endif
#include <smtlib2/theory.hpp>
#include <cassert>
#include <stdexcept>
#include <utility>

using namespace smtlib2;

template<class Exception, class Function>
void expect_throw(Function f) {
    bool caught = false;
    try { f(); } catch (const Exception&) { caught = true; }
    assert(caught);
}

int main() {
    const auto large = parse_numeral("1234567890123456789012345678901234567890");
    assert(large.to_string() == "1234567890123456789012345678901234567890");
    assert(parse_numeral("0").is_zero());
    for (auto bad : {"", "00", "012", "-1", "+1", " 1", "1 ", "0x10", "1.0", "1e5"})
        expect_throw<std::invalid_argument>([=] { parse_numeral(bad); });
    for (auto bad : {"", ".1", "1.", "00.1", "1.2.3", "-1.5", "+1.5", "1.0e1", "1.0 "})
        expect_throw<std::invalid_argument>([=] { parse_decimal(bad); });

    for (int a = -32; a <= 32; ++a) {
        for (int b = -8; b <= 8; ++b) {
            if (b == 0) continue;
            const int modulus = b < 0 ? -b : b;
            const int remainder = (a % modulus + modulus) % modulus;
            auto [q, r] = euclidean_divmod(Integer(a), Integer(b));
            assert(q == Integer((a - remainder) / b) && r == Integer(remainder));
        }
    }
    const Integer divisor("1234567890123456789");
    const auto wide_quotient = large * large;
    auto [q, r] = euclidean_divmod(-wide_quotient * divisor + 42, -divisor);
    assert(q == wide_quotient && r == 42);
    expect_throw<std::domain_error>([] { euclidean_divmod(1, 0); });

    const Rational half(1, 2), third(1, 3), zero, one(1);
    assert(half == Rational(-2, -4));
    assert(-half == Rational(2, -4));
    assert(zero == Rational(0, -17));
    assert(zero.denominator() == 1 && zero.numerator().is_zero());
    assert(half + third == Rational(5, 6));
    assert(half - third == Rational(1, 6));
    assert(half * third == Rational(1, 6));
    assert(half / -third == Rational(-3, 2));
    assert(half.floor() == 0 && (-half).floor() == -1 && one.floor() == 1);
    assert(!half.is_integer() && one.is_integer());
    assert(-half < zero && half > third && half >= half && third <= half && half != third);
    assert(Rational(large * 6, large * 8) == Rational(3, 4));
    expect_throw<std::domain_error>([] { Rational(1, 0); });
    expect_throw<std::domain_error>([&] { (void)(one / zero); });
    assert(parse_decimal("0.000") == zero);
    assert(parse_decimal("0.500") == half);
    assert(parse_decimal("0.00050") == Rational(1, 2000));
    assert(parse_decimal("1234567890123456789012345678901234567890.125") ==
           Rational(large * 8 + 1, 8));

    Rational source(2, 3);
    Rational dest(std::move(source));
    assert(source == dest);
    dest = half;
    assert(dest == half);

    const TheoryCapabilities caps;
    assert(!caps.integers && !caps.reals && !caps.bit_vectors &&
           !caps.arrays && !caps.floating_point && !caps.quantifiers);
}
