#include <Weighstone/CCINT.hpp>
#include <stdexcept>
#include <iostream>
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("test failed: " #condition); } while (false)
using namespace weighstone::ccint;

static void check_abstract_masks() {
    using Masks = weighstone::support::KnownBitMasks<std::uint64_t>;
    constexpr std::uint64_t mask = 7;
    const auto contains = [](Masks facts, std::uint64_t value) {
        return (facts.zero & value) == 0 && (facts.one & value) == facts.one;
    };
    for (std::uint64_t az=0; az<=mask; ++az) for (std::uint64_t ao=0; ao<=mask; ++ao) {
        if (az & ao) continue;
        for (std::uint64_t bz=0; bz<=mask; ++bz) for (std::uint64_t bo=0; bo<=mask; ++bo) {
            if (bz & bo) continue;
            const Masks a{az, ao}, b{bz, bo};
            const Masks results[] = {a.bit_and(b), a.bit_or(b), a.bit_xor(b)};
            Masks expected[] = {{mask, mask}, {mask, mask}, {mask, mask}};
            bool subset = true, disjoint = true;
            for (std::uint64_t x=0; x<=mask; ++x) for (std::uint64_t y=0; y<=mask; ++y) {
                if (!contains(a, x) || !contains(b, y)) continue;
                const std::uint64_t values[] = {x & y, x | y, x ^ y};
                for (unsigned i=0; i<3; ++i) {
                    expected[i].zero &= mask ^ values[i];
                    expected[i].one &= values[i];
                }
                subset &= (x & y) == x;
                disjoint &= (x & y) == 0;
            }
            for (unsigned i=0; i<3; ++i) {
                CHECK(results[i].zero == expected[i].zero);
                CHECK(results[i].one == expected[i].one);
            }
            CHECK(a.masked_by(b, mask) == subset);
            CHECK(a.disjoint(b, mask) == disjoint);
        }
    }
    constexpr auto all = ~std::uint64_t{0};
    constexpr auto high = std::uint64_t{1} << 63;
    CHECK((Masks{all ^ high, high}.all_known(all)));
    CHECK((Masks{all ^ high, 0}.masked_by(Masks{0, high}, all)));
    CHECK((Masks{all ^ high, 0}.disjoint(Masks{high, 0}, all)));
    CHECK((!Masks{all ^ high, 0}.disjoint(Masks{}, all)));
}

int main() {
    check_abstract_masks();
    // Exhaustive fixed-width arithmetic at small widths.
    for (unsigned w=1; w<=5; ++w) {
        const unsigned mod=1u<<w;
        const long long min=-(1LL<<(w-1)), max=(1LL<<(w-1))-1;
        for (unsigned i=0;i<mod;++i) for(unsigned j=0;j<mod;++j) {
            BitVector a(w,i), b(w,j);
            const long long ai=i<mod/2 ? static_cast<long long>(i) : static_cast<long long>(i)-mod;
            const long long bj=j<mod/2 ? static_cast<long long>(j) : static_cast<long long>(j)-mod;
            CHECK(a.signed_value()==ai && b.signed_value()==bj);
            CHECK(a.sign_bit()==(ai<0) && b.sign_bit()==(bj<0));
            auto plus=checked_add(a,b), minus=checked_sub(a,b), mul=checked_mul(a,b);
            CHECK(plus.value.unsigned_value()==(i+j)%mod);
            CHECK(minus.value.unsigned_value()==(i+mod-j)%mod);
            CHECK(mul.value.unsigned_value()==(i*j)%mod);
            CHECK(plus.unsigned_overflow==(i+j>=mod));
            CHECK(minus.unsigned_overflow==(i<j));
            CHECK(mul.unsigned_overflow==(i*j>=mod));
            CHECK(plus.signed_overflow==(ai+bj<min||ai+bj>max));
            CHECK(minus.signed_overflow==(ai-bj<min||ai-bj>max));
            CHECK(mul.signed_overflow==(ai*bj<min||ai*bj>max));
            CHECK(KnownBits::constant(a).bit_and(KnownBits::constant(b)).constant_value()==(a&b));
            CHECK(KnownBits::constant(a).bit_or(KnownBits::constant(b)).constant_value()==(a|b));
            CHECK(KnownBits::constant(a).bit_xor(KnownBits::constant(b)).constant_value()==(a^b));
        }
    }
    for (long long a=-13;a<=13;++a) for(long long b=-7;b<=7;++b) if(b!=0) {
        BigInt q=floor_div(BigInt(a),BigInt(b));
        BigInt r=BigInt(a)-q*BigInt(b);
        CHECK((b>0 && r>=0 && r<b)||(b<0 && r<=0 && r>b));
    }
    std::cout << "Exhaustive properties passed\n";
}
