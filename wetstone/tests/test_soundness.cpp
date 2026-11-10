#include "Wetstone.hpp"
#include <cassert>
#include <iostream>
using namespace wetstone;
int main(){
    for(int a=-12;a<=12;++a)for(int b=-12;b<=12;++b){
        auto ia=ai::Interval::point(a),ib=ai::Interval::point(b);
        assert(ia.add(ib).contains(a+b));
        assert(ia.mul(ib).contains(a*b));
        if(b){assert(ia.div(ib).contains(a/b));assert(ia.modulo(ib).contains(a%b));}
        auto sa=ai::Sign::from(ia),sb=ai::Sign::from(ib);
        assert(ai::Sign::from(ai::Interval::point(a+b)).leq(sa.add(sb)));
        assert(ai::Sign::from(ai::Interval::point(a*b)).leq(sa.mul(sb)));
        assert(ai::Parity::point(a+b).leq(ai::Parity::point(a).add(ai::Parity::point(b))));
        assert(ai::Parity::point(a*b).leq(ai::Parity::point(a).mul(ai::Parity::point(b))));
        for(int m1=1;m1<=6;++m1)for(int m2=1;m2<=6;++m2){
            auto c1=ai::Congruence::mod(m1,a),c2=ai::Congruence::mod(m2,b);
            auto meet=c1.meet(c2),join=c1.join(c2);
            assert(c1.leq(join)&&c2.leq(join));
            for(int x=-30;x<=30;++x)if(c1.contains(x)&&c2.contains(x))assert(meet.contains(x));
        }
    }
    for(unsigned a=0;a<256;++a)for(unsigned b=0;b<256;++b){
        auto ka=ai::KnownBits::constant(a,8),kb=ai::KnownBits::constant(b,8);
        assert(ka.bit_and(kb)==ai::KnownBits::constant(a&b,8));
        assert(ka.bit_or(kb)==ai::KnownBits::constant(a|b,8));
        assert(ka.bit_xor(kb)==ai::KnownBits::constant(a^b,8));
    }
    std::cout<<"Domain finite soundness tests passed\n";
}
