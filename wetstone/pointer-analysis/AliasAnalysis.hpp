#pragma once
#include "PointerAnalyzer.hpp"
namespace wetstone::pointer {
enum class Alias{NoAlias,MayAlias,MustAlias};
inline Alias alias(const PointsToResult& r,ir::ValueId a,ir::ValueId b){
    auto x=r.points_to(a),y=r.points_to(b);
    if(x.count(unknown_object)||y.count(unknown_object))return Alias::MayAlias;
    if(x.empty()||y.empty())return Alias::MayAlias; // missing evidence is not a no-alias proof
    if(x.size()==1&&y.size()==1&&x==y)return Alias::MustAlias;
    for(auto o:x)if(y.count(o))return Alias::MayAlias;
    return Alias::NoAlias;
}
}
