#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace wetstone::ai {
struct Interval {
    std::int64_t lo=std::numeric_limits<std::int64_t>::min();
    std::int64_t hi=std::numeric_limits<std::int64_t>::max();
    bool empty=false;
    Interval()=default;
    Interval(std::int64_t l,std::int64_t h):lo(l),hi(h),empty(l>h){}
    static Interval top(){return {};}
    static Interval bottom(){Interval x;x.empty=true;return x;}
    static Interval point(std::int64_t n){return {n,n};}
    bool is_top()const{return !empty&&lo==INT64_MIN&&hi==INT64_MAX;}
    bool is_bottom()const{return empty;}
    bool contains(std::int64_t x)const{return !empty&&lo<=x&&x<=hi;}
    bool leq(const Interval& b)const{return empty||( !b.empty && b.lo<=lo&&hi<=b.hi);}
    Interval join(const Interval& b)const{
        if(empty)return b;if(b.empty)return *this;return {std::min(lo,b.lo),std::max(hi,b.hi)};
    }
    Interval meet(const Interval& b)const{
        if(empty||b.empty)return bottom();return {std::max(lo,b.lo),std::min(hi,b.hi)};
    }
    Interval widen(const Interval& b)const{
        if(empty)return b;if(b.empty)return *this;
        return {b.lo<lo?INT64_MIN:lo,b.hi>hi?INT64_MAX:hi};
    }
    Interval narrow(const Interval& b)const{
        if(empty||b.empty)return bottom();return {lo==INT64_MIN?b.lo:lo,hi==INT64_MAX?b.hi:hi};
    }
    static std::int64_t clamp(__int128 x) {
        if(x<INT64_MIN)return INT64_MIN;if(x>INT64_MAX)return INT64_MAX;
        return static_cast<std::int64_t>(x);
    }
    Interval add(const Interval& b)const {
        if(empty||b.empty)return bottom();
        return {clamp(static_cast<__int128>(lo)+b.lo),clamp(static_cast<__int128>(hi)+b.hi)};
    }
    Interval neg()const {
        if(empty)return bottom();
        return {clamp(-static_cast<__int128>(hi)),clamp(-static_cast<__int128>(lo))};
    }
    Interval sub(const Interval& b)const {return add(b.neg());}
    Interval mul(const Interval& b)const {
        if(empty||b.empty)return bottom();
        const __int128 p[]={static_cast<__int128>(lo)*b.lo,static_cast<__int128>(lo)*b.hi,
            static_cast<__int128>(hi)*b.lo,static_cast<__int128>(hi)*b.hi};
        return {clamp(*std::min_element(p,p+4)),clamp(*std::max_element(p,p+4))};
    }
    Interval div(const Interval& b)const {
        if(empty||b.empty)return bottom();
        if(b.contains(0))return top();
        const __int128 p[]={static_cast<__int128>(lo)/b.lo,static_cast<__int128>(lo)/b.hi,
            static_cast<__int128>(hi)/b.lo,static_cast<__int128>(hi)/b.hi};
        return {clamp(*std::min_element(p,p+4)),clamp(*std::max_element(p,p+4))};
    }
    Interval modulo(const Interval& b)const {
        if(empty||b.empty)return bottom();
        if(b.contains(0))return top();
        auto m=std::max(static_cast<__int128>(b.lo)<0?-static_cast<__int128>(b.lo):static_cast<__int128>(b.lo),
                        static_cast<__int128>(b.hi)<0?-static_cast<__int128>(b.hi):static_cast<__int128>(b.hi))-1;
        auto max=clamp(m);
        if(lo>=0)return {0,std::min(hi,max)};
        if(hi<=0)return {std::max(lo,-max),0};
        return {-max,max};
    }
    friend bool operator==(const Interval& a,const Interval& b){return a.empty&&b.empty || (!a.empty&&!b.empty&&a.lo==b.lo&&a.hi==b.hi);}
};
}
