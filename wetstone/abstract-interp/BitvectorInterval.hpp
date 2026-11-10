#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
namespace wetstone::ai {
// Unsigned modular interval [low,high] with low<=high. Operations that may wrap
// conservatively degenerate to top rather than model a disjunctive result; this
// stays sound (over-approximates) at the cost of precision. Use the KnownBits
// domain or the TypedInt reduced product to recover per-bit information.
struct BitvectorInterval {
    unsigned width=32;std::uint64_t low=0,high=UINT32_MAX;bool empty=false;
    static std::uint64_t mask(unsigned width){if(width==0||width>64)throw std::invalid_argument("bit width");return width==64?UINT64_MAX:((1ULL<<width)-1);}
    static BitvectorInterval top(unsigned w=32){return {w,0,mask(w),false};}
    static BitvectorInterval bottom(unsigned w=32){return {w,0,0,true};}
    static BitvectorInterval point(std::uint64_t v,unsigned w=32){auto m=mask(w);return {w,v&m,v&m,false};}
    void check(const BitvectorInterval& b)const{if(width!=b.width)throw std::invalid_argument("width mismatch");}
    bool is_top()const{return !empty&&low==0&&high==mask(width);}
    bool is_bottom()const{return empty;}
    bool contains(std::uint64_t x)const{return !empty&&low<=x&&x<=high;}
    bool leq(const BitvectorInterval& b)const{check(b);return empty||(!b.empty&&low>=b.low&&high<=b.high);}
    BitvectorInterval join(const BitvectorInterval& b)const{check(b);if(empty)return b;if(b.empty)return *this;return {width,std::min(low,b.low),std::max(high,b.high),false};}
    BitvectorInterval meet(const BitvectorInterval& b)const{check(b);if(empty||b.empty)return bottom(width);auto l=std::max(low,b.low),h=std::min(high,b.high);return l>h?bottom(width):BitvectorInterval{width,l,h,false};}
    BitvectorInterval widen(const BitvectorInterval& b)const{check(b);if(empty)return b;if(b.empty)return *this;return {width,b.low<low?0:low,b.high>high?mask(width):high,false};}
    BitvectorInterval add(const BitvectorInterval& b)const{
        check(b);if(empty||b.empty)return bottom(width);
        const auto max=mask(width);const auto sum=static_cast<unsigned __int128>(high)+b.high;
        if(sum>max)return top(width);
        return {width,low+b.low,high+b.high,false};
    }
    BitvectorInterval sub(const BitvectorInterval& b)const{
        check(b);if(empty||b.empty)return bottom(width);
        // Result in [low-b.high, high-b.low]; sound only when no underflow past 0.
        if(low>=b.high)return {width,low-b.high,high-b.low,false};
        return top(width);
    }
    BitvectorInterval mul(const BitvectorInterval& b)const{
        check(b);if(empty||b.empty)return bottom(width);
        const auto max=mask(width);
        unsigned __int128 p[4]={(unsigned __int128)low*b.low,(unsigned __int128)low*b.high,
            (unsigned __int128)high*b.low,(unsigned __int128)high*b.high};
        auto lo=*std::min_element(p,p+4),hi=*std::max_element(p,p+4);
        if(hi>max)return top(width);
        return {width,static_cast<std::uint64_t>(lo),static_cast<std::uint64_t>(hi),false};
    }
    BitvectorInterval div(const BitvectorInterval& b)const{
        check(b);if(empty||b.empty)return bottom(width);
        if(b.low==0)return top(width); // divisor may be zero
        // Unsigned division is monotonic non-decreasing in the dividend and
        // non-increasing in the divisor, so the extrema sit at the corners.
        return {width,low/b.high,high/b.low,false};
    }
    BitvectorInterval modulo(const BitvectorInterval& b)const{
        check(b);if(empty||b.empty)return bottom(width);
        if(b.low==0)return top(width);
        // a % b lies in [0, b.high-1] and cannot exceed a itself.
        return {width,0,std::min(high,b.high-1),false};
    }
    BitvectorInterval neg()const{
        if(empty)return bottom(width);
        if(low==0&&high==0)return point(0,width);
        // -x mod 2^w = 2^w - x. For low>0 this is the non-wrapping range
        // [2^w - high, 2^w - low]; if 0 is in range the result wraps to top.
        if(low>0){
            unsigned __int128 two_w=static_cast<unsigned __int128>(mask(width))+1;
            return {width,static_cast<std::uint64_t>(two_w-high),static_cast<std::uint64_t>(two_w-low),false};
        }
        return top(width);
    }
    BitvectorInterval shl(const BitvectorInterval& b)const{
        check(b);if(empty||b.empty)return bottom(width);
        if(b.low!=b.high)return top(width); // unknown shift amount
        const auto k=b.low;const auto max=mask(width);
        if(k>=width)return point(0,width);
        unsigned __int128 lo=static_cast<unsigned __int128>(low)<<k;
        unsigned __int128 hi=static_cast<unsigned __int128>(high)<<k;
        if(hi>max)return top(width);
        return {width,static_cast<std::uint64_t>(lo),static_cast<std::uint64_t>(hi),false};
    }
    BitvectorInterval shr(const BitvectorInterval& b)const{
        check(b);if(empty||b.empty)return bottom(width);
        if(b.low!=b.high)return top(width);
        const auto k=b.low;
        if(k>=width)return point(0,width);
        return {width,low>>k,high>>k,false};
    }
    // Bitwise ops on ranges are imprecise without per-bit knowledge; the precise
    // result requires the KnownBits domain or the TypedInt reduced product.
    BitvectorInterval bit_and(const BitvectorInterval& b)const{check(b);if(empty||b.empty)return bottom(width);if(low==high&&b.low==b.high)return point(low&b.low,width);return top(width);}
    BitvectorInterval bit_or(const BitvectorInterval& b)const{check(b);if(empty||b.empty)return bottom(width);if(low==high&&b.low==b.high)return point(low|b.low,width);return top(width);}
    BitvectorInterval bit_xor(const BitvectorInterval& b)const{check(b);if(empty||b.empty)return bottom(width);if(low==high&&b.low==b.high)return point(low^b.low,width);return top(width);}
    BitvectorInterval bit_not()const{if(empty)return bottom(width);if(low==high)return point(~low&mask(width),width);return top(width);}
    // Comparisons yield a width-1 interval over {0,1}.
    BitvectorInterval equal(const BitvectorInterval& b)const{check(b);if(empty||b.empty)return bottom(1);if(high<b.low||b.high<low)return point(0,1);if(low==high&&b.low==b.high&&low==b.low)return point(1,1);return {1,0,1,false};}
    BitvectorInterval not_equal(const BitvectorInterval& b)const{check(b);if(empty||b.empty)return bottom(1);auto e=equal(b);if(e.low==e.high)return point(1-e.low,1);return {1,0,1,false};}
    BitvectorInterval less(const BitvectorInterval& b)const{check(b);if(empty||b.empty)return bottom(1);if(high<b.low)return point(1,1);if(low>=b.high)return point(0,1);return {1,0,1,false};}
    BitvectorInterval less_equal(const BitvectorInterval& b)const{check(b);if(empty||b.empty)return bottom(1);if(high<=b.low)return point(1,1);if(low>b.high)return point(0,1);return {1,0,1,false};}
    BitvectorInterval greater(const BitvectorInterval& b)const{check(b);return b.less(*this);}
    BitvectorInterval greater_equal(const BitvectorInterval& b)const{check(b);return b.less_equal(*this);}
    friend bool operator==(const BitvectorInterval&,const BitvectorInterval&)=default;
};
} // namespace wetstone::ai
