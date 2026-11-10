#pragma once
#include "../abstract-interp/Transfer.hpp"
namespace wetstone::path {
enum class Feasibility{Impossible,Possible,Certain};
inline Feasibility branch_true(const ai::Environment& env,const ir::Operand& o){
    auto x=ai::operand(env,o);if(x.empty||x==ai::Interval::point(0))return Feasibility::Impossible;
    if(!x.contains(0))return Feasibility::Certain;return Feasibility::Possible;
}
}
