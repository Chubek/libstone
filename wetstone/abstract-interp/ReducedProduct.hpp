#pragma once
#include "Congruence.hpp"
#include "Interval.hpp"
namespace wetstone::ai {
struct IntervalCongruence {
    Interval interval=Interval::top();Congruence congruence=Congruence::top();
    static IntervalCongruence top(){return {};}
    static IntervalCongruence bottom(){return {Interval::bottom(),Congruence::bottom()};}
    void reduce(){
        if(interval.empty||congruence.empty){*this=bottom();return;}
        if(congruence.modulus==0){interval=interval.meet(Interval::point(congruence.residue));if(interval.empty)*this=bottom();return;}
        if(congruence.modulus==1)return;
        const auto m=congruence.modulus,r=congruence.residue;
        auto rem=[&](__int128 x){auto y=x%m;return y<0?y+m:y;};
        __int128 lo=static_cast<__int128>(interval.lo)+rem(static_cast<__int128>(r)-interval.lo);
        __int128 hi=static_cast<__int128>(interval.hi)-rem(static_cast<__int128>(interval.hi)-r);
        if(lo>hi){*this=bottom();return;}
        interval={static_cast<std::int64_t>(lo),static_cast<std::int64_t>(hi)};
        if(lo==hi)congruence=Congruence::constant(static_cast<std::int64_t>(lo));
    }
    bool leq(const IntervalCongruence& b)const{return interval.leq(b.interval)&&congruence.leq(b.congruence);}
    IntervalCongruence join(const IntervalCongruence& b)const{auto r=IntervalCongruence{interval.join(b.interval),congruence.join(b.congruence)};r.reduce();return r;}
    IntervalCongruence meet(const IntervalCongruence& b)const{auto r=IntervalCongruence{interval.meet(b.interval),congruence.meet(b.congruence)};r.reduce();return r;}
    IntervalCongruence widen(const IntervalCongruence& b)const{auto r=IntervalCongruence{interval.widen(b.interval),congruence.widen(b.congruence)};r.reduce();return r;}
    friend bool operator==(const IntervalCongruence&,const IntervalCongruence&)=default;
};
}
