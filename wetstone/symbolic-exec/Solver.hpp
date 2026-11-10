#pragma once
#include "BooleanCNF.hpp"
#include <cstdint>
#include <map>
#include <memory>
#if __has_include(<SatieCDCL.hpp>)
#include <SatieCDCL.hpp>
#define WETSTONE_HAS_SATIE 1
#else
#define WETSTONE_HAS_SATIE 0
#endif
namespace wetstone::symbolic {
enum class CheckResult{Sat,Unsat,Unknown};
struct CheckReport {CheckResult status=CheckResult::Unknown;std::map<std::string,bool> booleans;};
struct Solver {
    virtual ~Solver()=default;
    virtual CheckReport check(const PathCondition&)=0;
};
// Satie is deliberately the sole SAT implementation: never silently claim SAT
// from an absent solver or by treating theory atoms as independent Booleans.
class SatieSolver final:public Solver {
public:
    CheckReport check(const PathCondition& conditions)override{
        BooleanCNF enc;for(const auto& x:conditions)enc.assert_true(x);
        if(!enc.supported)return {};
#if WETSTONE_HAS_SATIE
        satie::CNF problem(enc.clauses);
        const auto result=satie::solve_cdcl(problem);
        if(result.unsatisfiable())return {CheckResult::Unsat,{}};
        if(!result.satisfiable())return {};
        CheckReport report;report.status=CheckResult::Sat;
        for(const auto& [key,variable]:enc.variables){auto v=result.assignment.get_var(variable);
            if(v==satie::Value::TRUE)report.booleans[key]=true;
            else if(v==satie::Value::FALSE)report.booleans[key]=false;}
        return report;
#else
        (void)enc;return {};
#endif
    }
};
}
