#pragma once
#include "Interval.hpp"
#include <set>
namespace wetstone::ai {
inline Interval widen_with_thresholds(const Interval& old,const Interval& next,const std::set<std::int64_t>& thresholds){
    if(old.empty)return next;if(next.empty)return old;
    auto low=old.lo,high=old.hi;
    if(next.lo<old.lo){low=INT64_MIN;auto it=thresholds.upper_bound(next.lo);
        if(it!=thresholds.begin()){--it;low=*it;}}
    if(next.hi>old.hi){high=INT64_MAX;auto it=thresholds.lower_bound(next.hi);
        if(it!=thresholds.end())high=*it;}
    return {low,high};
}
}
