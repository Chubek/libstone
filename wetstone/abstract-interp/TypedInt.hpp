#pragma once
#include "BitvectorInterval.hpp"
#include "KnownBits.hpp"
#include "AbstractDomain.hpp"
#include <cstdint>
#include <stdexcept>
namespace wetstone::ai {
// Reduced product of an unsigned bitvector interval and the known-bits lattice.
// range bounds the value; bits tracks per-bit zeroes/ones. reduce() propagates
// fully-known constants between the two components and detects contradictions at
// singletons; no other cross-derivation is attempted, so non-constant states are
// sound (if imprecise). Arithmetic delegates to range and drops bit precision;
// bitwise delegates to bits and recovers a precise range when bits become fully
// determined. Together this implements a typed integer domain with width and
// signed/unsigned-magnitude semantics (AGENTS follow-on item 1).
struct TypedInt {
    unsigned width=32;BitvectorInterval range;KnownBits bits;bool empty=false;
    static TypedInt top(unsigned w=32){return {w,BitvectorInterval::top(w),KnownBits::top(w),false};}
    static TypedInt bottom(unsigned w=32){return {w,BitvectorInterval::bottom(w),KnownBits::bottom(w),true};}
    static TypedInt point(std::uint64_t v,unsigned w=32){return {w,BitvectorInterval::point(v,w),KnownBits::constant(v,w),false};}
    static TypedInt from_bits(const KnownBits& b){TypedInt r{b.width,BitvectorInterval::top(b.width),b,b.empty};r.reduce();return r;}
    bool is_bottom()const{return empty||range.empty||bits.empty;}
    bool is_top()const{return !empty&&range.is_top()&&bits.zero==0&&bits.one==0;}
    void compatible(const TypedInt& b)const{if(width!=b.width)throw std::invalid_argument("TypedInt width mismatch");}
    void reduce(){
        if(empty||range.empty||bits.empty){*this=bottom(width);return;}
        const auto m=BitvectorInterval::mask(width);
        const auto known=bits.zero|bits.one;
        if((known&m)==m){                       // all bits known => singleton
            const auto v=bits.one&m;
            if(v<range.low||v>range.high){*this=bottom(width);return;}
            range=BitvectorInterval::point(v,width);
            return;
        }
        if(range.low==range.high){              // range is a singleton => constrain bits
            const auto v=range.low;
            if((bits.zero&v)!=0||(bits.one&(~v&m))!=0){*this=bottom(width);return;}
            bits=KnownBits::constant(v,width);
        }
    }
    bool leq(const TypedInt& b)const{compatible(b);return empty||(!b.empty&&range.leq(b.range)&&bits.leq(b.bits));}
    TypedInt join(const TypedInt& b)const{compatible(b);if(empty)return b;if(b.empty)return *this;
        TypedInt r{width,range.join(b.range),bits.join(b.bits),false};r.reduce();return r;}
    TypedInt meet(const TypedInt& b)const{compatible(b);if(empty||b.empty)return bottom(width);
        TypedInt r{width,range.meet(b.range),bits.meet(b.bits),false};r.reduce();return r;}
    TypedInt widen(const TypedInt& b)const{compatible(b);if(empty)return b;if(b.empty)return *this;
        TypedInt r{width,range.widen(b.range),bits.widen(b.bits),false};r.reduce();return r;}
    friend bool operator==(const TypedInt&,const TypedInt&)=default;

    // Arithmetic uses the range component; per-bit facts are dropped because the
    // known-bits lattice has no exact additive/multiplicative transfer.
    TypedInt add(const TypedInt& b)const{compatible(b);if(is_bottom()||b.is_bottom())return bottom(width);
        TypedInt r{width,range.add(b.range),KnownBits::top(width),false};r.reduce();return r;}
    TypedInt sub(const TypedInt& b)const{compatible(b);if(is_bottom()||b.is_bottom())return bottom(width);
        TypedInt r{width,range.sub(b.range),KnownBits::top(width),false};r.reduce();return r;}
    TypedInt mul(const TypedInt& b)const{compatible(b);if(is_bottom()||b.is_bottom())return bottom(width);
        TypedInt r{width,range.mul(b.range),KnownBits::top(width),false};r.reduce();return r;}
    TypedInt div(const TypedInt& b)const{compatible(b);if(is_bottom()||b.is_bottom())return bottom(width);
        TypedInt r{width,range.div(b.range),KnownBits::top(width),false};r.reduce();return r;}
    TypedInt modulo(const TypedInt& b)const{compatible(b);if(is_bottom()||b.is_bottom())return bottom(width);
        TypedInt r{width,range.modulo(b.range),KnownBits::top(width),false};r.reduce();return r;}
    TypedInt neg()const{if(is_bottom())return bottom(width);
        TypedInt r{width,range.neg(),KnownBits::top(width),false};r.reduce();return r;}
    TypedInt shl(const TypedInt& b)const{compatible(b);if(is_bottom()||b.is_bottom())return bottom(width);
        TypedInt r{width,range.shl(b.range),KnownBits::top(width),false};r.reduce();return r;}
    TypedInt shr(const TypedInt& b)const{compatible(b);if(is_bottom()||b.is_bottom())return bottom(width);
        TypedInt r{width,range.shr(b.range),KnownBits::top(width),false};r.reduce();return r;}
    // Bitwise uses the bits component; the range collapses to a singleton only
    // when bits become fully determined.
    TypedInt bit_and(const TypedInt& b)const{compatible(b);if(is_bottom()||b.is_bottom())return bottom(width);
        TypedInt r{width,BitvectorInterval::top(width),bits.bit_and(b.bits),false};r.reduce();return r;}
    TypedInt bit_or(const TypedInt& b)const{compatible(b);if(is_bottom()||b.is_bottom())return bottom(width);
        TypedInt r{width,BitvectorInterval::top(width),bits.bit_or(b.bits),false};r.reduce();return r;}
    TypedInt bit_xor(const TypedInt& b)const{compatible(b);if(is_bottom()||b.is_bottom())return bottom(width);
        TypedInt r{width,BitvectorInterval::top(width),bits.bit_xor(b.bits),false};r.reduce();return r;}
    TypedInt bit_not()const{if(is_bottom())return bottom(width);
        TypedInt r{width,BitvectorInterval::top(width),bits.bit_not(),false};r.reduce();return r;}
};
static_assert(AbstractDomain<TypedInt>,"TypedInt must model AbstractDomain");
} // namespace wetstone::ai
