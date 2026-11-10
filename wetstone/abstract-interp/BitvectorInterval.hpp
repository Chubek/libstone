#pragma once
#include <cstdint>
#include <stdexcept>
namespace wetstone::ai {
// Unsigned modular interval; wrapping joins conservatively to top.
struct BitvectorInterval {
    unsigned width=32;std::uint64_t low=0,high=UINT32_MAX;bool empty=false;
    static std::uint64_t mask(unsigned width){if(width==0||width>64)throw std::invalid_argument("bit width");return width==64?UINT64_MAX:((1ULL<<width)-1);}
    static BitvectorInterval top(unsigned w=32){return {w,0,mask(w),false};}
    static BitvectorInterval bottom(unsigned w=32){return {w,0,0,true};}
    static BitvectorInterval point(std::uint64_t v,unsigned w=32){auto m=mask(w);return {w,v&m,v&m,false};}
    void check(const BitvectorInterval& b)const{if(width!=b.width)throw std::invalid_argument("width mismatch");}
    bool leq(const BitvectorInterval& b)const{check(b);return empty||(!b.empty&&low>=b.low&&high<=b.high);}
    BitvectorInterval join(const BitvectorInterval& b)const{check(b);if(empty)return b;if(b.empty)return *this;return {width,std::min(low,b.low),std::max(high,b.high),false};}
    BitvectorInterval meet(const BitvectorInterval& b)const{check(b);if(empty||b.empty)return bottom(width);auto l=std::max(low,b.low),h=std::min(high,b.high);return l>h?bottom(width):BitvectorInterval{width,l,h,false};}
    BitvectorInterval widen(const BitvectorInterval& b)const{check(b);if(empty)return b;if(b.empty)return *this;return {width,b.low<low?0:low,b.high>high?mask(width):high,false};}
    BitvectorInterval add(const BitvectorInterval& b)const{
        check(b);if(empty||b.empty)return bottom(width);
        // Non-wrapping if upper sum <= mask; otherwise wrap may produce a disjunction.
        const auto max=mask(width);const auto sum=static_cast<unsigned __int128>(high)+b.high;
        if(sum>max)return top(width);
        return {width,low+b.low,high+b.high,false};
    }
    friend bool operator==(const BitvectorInterval&,const BitvectorInterval&)=default;
};
}
