#pragma once
#include "../symbolic-exec/Solver.hpp"
#if __has_include(<SatieLIA.hpp>)
#include <SatieLIA.hpp>
#define WETSTONE_HAS_SATIE_LIA 1
#else
#define WETSTONE_HAS_SATIE_LIA 0
#endif
namespace wetstone::integration {
struct LinearInequality {
    std::vector<std::pair<std::string,std::int64_t>> coefficients;
    std::int64_t bound=0;
};
struct LinearResult {
    symbolic::CheckResult status=symbolic::CheckResult::Unknown;
    std::map<std::string,std::int64_t> model;
};
inline LinearResult check_linear_integer(const std::vector<LinearInequality>& constraints,int width=16){
#if WETSTONE_HAS_SATIE_LIA
    satie::lia::LIASolver solver;solver.set_width(width);
    std::map<std::string,int> variables;
    for(const auto& c:constraints)for(const auto& [key,_]:c.coefficients)
        if(!variables.count(key))variables[key]=solver.add_var(key);
    for(const auto& c:constraints){std::vector<std::pair<int,std::int64_t>> row;
        for(const auto& [key,coefficient]:c.coefficients)row.push_back({variables.at(key),coefficient});
        solver.add_le(row,c.bound);
    }
    auto answer=solver.check();LinearResult result;
    if(answer.unsatisfiable())result.status=symbolic::CheckResult::Unsat;
    else if(answer.satisfiable()){result.status=symbolic::CheckResult::Sat;
        for(const auto& [key,id]:variables)result.model[key]=solver.value(id);}
    return result;
#else
    (void)constraints;(void)width;return {};
#endif
}
}
