#ifdef NDEBUG
#undef NDEBUG
#endif
#include <smtlib2/bitvector.hpp>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>

using smtlib2::BitVector;
using smtlib2::Integer;

template<class Exception, class Function>
void expect_throw(Function f) {
    bool caught = false;
    try { f(); } catch (const Exception&) { caught = true; }
    assert(caught);
}

int order(int a, int b) { return (a > b) - (a < b); }

void exhaustive_small_widths() {
    for (unsigned width = 1; width <= 6; ++width) {
        const int modulus = 1 << width;
        const int mask = modulus - 1;
        const auto signed_of = [&](int n) { return n < modulus / 2 ? n : n - modulus; };
        const auto expect = [&](const BitVector& result, int value) {
            assert(result.width() == width);
            assert(result.unsigned_value() == ((value % modulus + modulus) % modulus));
        };
        for (int a = 0; a < modulus; ++a) {
            const BitVector av(width, a);
            const int sa = signed_of(a);
            assert(av.signed_value() == sa);
            expect(BitVector(width, a - 2 * modulus), a);
            expect(BitVector(width, a + 2 * modulus), a);
            expect(-av, -a);
            expect(~av, mask ^ a);
            for (int b = 0; b < modulus; ++b) {
                const BitVector bv(width, b);
                const int sb = signed_of(b);
                assert((av == bv) == (a == b));
                assert(av.unsigned_compare(bv) == order(a, b));
                assert(av.signed_compare(bv) == order(sa, sb));
                expect(av + bv, a + b);
                expect(av - bv, a - b);
                expect(av * bv, a * b);
                expect(av & bv, a & b);
                expect(av | bv, a | b);
                expect(av ^ bv, a ^ b);
                expect(av.udiv(bv), b ? a / b : mask);
                expect(av.urem(bv), b ? a % b : a);
                expect(av.sdiv(bv), sb ? sa / sb : sa < 0 ? 1 : -1);
                const int signed_rem = sb ? sa % sb : sa;
                expect(av.srem(bv), signed_rem);
                expect(av.smod(bv), sb && signed_rem && (signed_rem < 0) != (sb < 0)
                                        ? signed_rem + sb : signed_rem);
                if (b >= static_cast<int>(width)) {
                    expect(av.shl(bv), 0);
                    expect(av.lshr(bv), 0);
                    expect(av.ashr(bv), sa < 0 ? -1 : 0);
                } else {
                    expect(av.shl(bv), a << b);
                    expect(av.lshr(bv), a >> b);
                    const int scale = 1 << b;
                    expect(av.ashr(bv), sa < 0 ? (sa - (scale - 1)) / scale : sa / scale);
                }
            }
            for (unsigned high = 0; high < width; ++high) {
                for (unsigned low = 0; low <= high; ++low) {
                    const auto result = av.extract(high, low);
                    assert(result.width() == high - low + 1);
                    assert(result.unsigned_value() == ((a >> low) & ((1 << result.width()) - 1)));
                }
                if (high + 1 < width)
                    assert(av.extract(width - 1, high + 1).concat(av.extract(high, 0)) == av);
            }
        }
    }
}

void wide_values() {
    const auto p32 = Integer(1) << 32;
    const auto p64 = Integer(1) << 64;
    const auto p128 = Integer(1) << 128;
    const auto p129 = Integer(1) << 129;
    const BitVector a(129, p128 + p64 + 3), b(129, p64 + p32 + 6);
    const BitVector zero(129), ones(129, -1), min(129, p128);
    assert(a.signed_value() == -p128 + p64 + 3);
    assert(BitVector(129, -(p129 + 3)) == BitVector(129, p129 - 3));
    assert(BitVector(129, -p129) == zero);
    assert(ones + BitVector(129, 1) == zero);
    assert(min * BitVector(129, 3) == min);
    assert((a & b).unsigned_value() == p64 + 2);
    assert((a | b).unsigned_value() == p128 + p64 + p32 + 7);
    assert((a ^ b).unsigned_value() == p128 + p32 + 5);
    assert((a | ~a) == ones && (a & ~a) == zero);
    assert(a.extract(128, 64) == BitVector(65, p64 + 1));
    assert(a.extract(128, 64).concat(a.extract(63, 0)) == a);
    assert(a.shl(BitVector(129, 64)) == BitVector(129, p128 + 3 * p64));
    assert(a.lshr(BitVector(129, 64)) == BitVector(129, p64 + 1));
    assert(a.ashr(BitVector(129, 64)) == BitVector(129, -p64 + 1));
    assert(a.shl(BitVector(129, p128)) == zero);
    assert(a.lshr(BitVector(129, p128)) == zero);
    assert(a.ashr(BitVector(129, p128)) == ones);
    assert(b.ashr(BitVector(129, p128)) == zero);
    assert(min.sdiv(ones) == min && min.srem(ones) == zero && min.smod(ones) == zero);

    const auto divisor = p32 + 3;
    const auto quotient = p64 + 5;
    const auto dividend = divisor * quotient + 1;
    const BitVector u(129, dividend), d(129, divisor);
    assert(u.udiv(d) == BitVector(129, quotient) && u.urem(d) == BitVector(129, 1));
    assert((-u).sdiv(d) == BitVector(129, -quotient));
    assert((-u).srem(d) == ones);
    assert((-u).smod(d) == BitVector(129, divisor - 1));
}

void validation() {
    expect_throw<std::invalid_argument>([] { (void)BitVector(0); });
    expect_throw<std::length_error>([] { (void)BitVector(BitVector::max_width + 1); });
    expect_throw<std::length_error>([] { (void)BitVector(std::numeric_limits<std::size_t>::max()); });
    const BitVector max(BitVector::max_width), a(8, 1), b(7, 1);
    assert(max.width() == BitVector::max_width);
    expect_throw<std::length_error>([&] { max.concat(a); });
    expect_throw<std::length_error>([&] { a.concat(max); });
    expect_throw<std::out_of_range>([&] { a.extract(8, 0); });
    expect_throw<std::out_of_range>([&] { a.extract(2, 3); });
    expect_throw<std::out_of_range>([&] { a.extract(7, std::numeric_limits<std::size_t>::max()); });
    assert(a != b);
    expect_throw<std::invalid_argument>([&] { a.unsigned_compare(b); });
    expect_throw<std::invalid_argument>([&] { a.signed_compare(b); });
    expect_throw<std::invalid_argument>([&] { (void)(a + b); });
    expect_throw<std::invalid_argument>([&] { (void)(a - b); });
    expect_throw<std::invalid_argument>([&] { (void)(a * b); });
    expect_throw<std::invalid_argument>([&] { (void)(a & b); });
    expect_throw<std::invalid_argument>([&] { (void)(a | b); });
    expect_throw<std::invalid_argument>([&] { (void)(a ^ b); });
    expect_throw<std::invalid_argument>([&] { a.shl(b); });
    expect_throw<std::invalid_argument>([&] { a.lshr(b); });
    expect_throw<std::invalid_argument>([&] { a.ashr(b); });
    expect_throw<std::invalid_argument>([&] { a.udiv(b); });
    expect_throw<std::invalid_argument>([&] { a.urem(b); });
    expect_throw<std::invalid_argument>([&] { a.sdiv(b); });
    expect_throw<std::invalid_argument>([&] { a.srem(b); });
    expect_throw<std::invalid_argument>([&] { a.smod(b); });

    BitVector source = a;
    BitVector dest(std::move(source));
    assert(dest == a && source == BitVector(a.width()));
    source = b;
    dest = std::move(source);
    assert(dest == b && source == BitVector(b.width()));
}

int main() {
    exhaustive_small_widths();
    wide_values();
    validation();
}
