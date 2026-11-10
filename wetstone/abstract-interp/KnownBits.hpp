#pragma once
#include <cstdint>
#include <stdexcept>
namespace wetstone::ai {
struct KnownBits {
    unsigned width=64;std::uint64_t zero=0,one=0;bool empty=false;
    static std::uint64_t bits(unsigned w){if(w==0||w>64)throw std::invalid_argument("bit width must be 1..64");return w==64?UINT64_MAX:((1ULL<<w)-1);}
    static KnownBits top(unsigned w=64){bits(w);return {w,0,0,false};}
    static KnownBits bottom(unsigned w=64){auto x=top(w);x.empty=true;return x;}
    static KnownBits constant(std::uint64_t n,unsigned w=64){auto m=bits(w);return {w,(~n)&m,n&m,false};}
    void compatible(const KnownBits& b)const{if(width!=b.width)throw std::invalid_argument("known-bits width mismatch");}
    bool leq(const KnownBits& b)const{compatible(b);return empty||(!b.empty&&(zero&b.zero)==b.zero&&(one&b.one)==b.one);}
    KnownBits join(const KnownBits& b)const{compatible(b);if(empty)return b;if(b.empty)return *this;return {width,zero&b.zero,one&b.one,false};}
    KnownBits meet(const KnownBits& b)const{compatible(b);if(empty||b.empty)return bottom(width);auto z=zero|b.zero,o=one|b.one;return (z&o)?bottom(width):KnownBits{width,z,o,false};}
    KnownBits widen(const KnownBits& b)const{return join(b);}
    KnownBits bit_and(const KnownBits& b)const{compatible(b);if(empty||b.empty)return bottom(width);return {width,(zero|b.zero)&bits(width),one&b.one,false};}
    KnownBits bit_or(const KnownBits& b)const{compatible(b);if(empty||b.empty)return bottom(width);return {width,zero&b.zero,(one|b.one)&bits(width),false};}
    KnownBits bit_xor(const KnownBits& b)const{compatible(b);if(empty||b.empty)return bottom(width);
        return {width,(zero&b.zero)|(one&b.one),(one&b.zero)|(zero&b.one),false};}
    KnownBits bit_not()const{return empty?bottom(width):KnownBits{width,one,zero,false};}
    friend bool operator==(const KnownBits&,const KnownBits&)=default;
};
}
