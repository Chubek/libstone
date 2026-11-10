#pragma once
#include "ReducedProduct.hpp"
namespace wetstone::ai {
// Strided interval, represented by a reduced product with congruences.
class StridedInterval : public IntervalCongruence {
public:
    using IntervalCongruence::IntervalCongruence;
    static StridedInterval top(){return {};}
    static StridedInterval bottom(){StridedInterval r;r.interval=Interval::bottom();r.congruence=Congruence::bottom();return r;}
    static StridedInterval make(std::int64_t low,std::int64_t high,std::int64_t stride,std::int64_t offset){
        StridedInterval x;x.interval=Interval{low,high};x.congruence=stride?Congruence::mod(stride,offset):Congruence::constant(offset);
        x.reduce();return x;
    }
    StridedInterval join(const StridedInterval& b)const{StridedInterval r;r.interval=interval.join(b.interval);r.congruence=congruence.join(b.congruence);r.reduce();return r;}
    StridedInterval meet(const StridedInterval& b)const{StridedInterval r;r.interval=interval.meet(b.interval);r.congruence=congruence.meet(b.congruence);r.reduce();return r;}
    StridedInterval widen(const StridedInterval& b)const{StridedInterval r;r.interval=interval.widen(b.interval);r.congruence=congruence.widen(b.congruence);r.reduce();return r;}
    bool leq(const StridedInterval& b)const{return interval.leq(b.interval)&&congruence.leq(b.congruence);}
    friend bool operator==(const StridedInterval&,const StridedInterval&)=default;
};
}
