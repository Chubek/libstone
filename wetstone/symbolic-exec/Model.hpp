#pragma once
#include "Solver.hpp"
namespace wetstone::symbolic {
inline std::optional<bool> bool_value(const CheckReport& result,const std::string& name){
    if(result.status!=CheckResult::Sat)return std::nullopt;
    auto it=result.booleans.find(name);if(it==result.booleans.end())return std::nullopt;return it->second;
}
}
