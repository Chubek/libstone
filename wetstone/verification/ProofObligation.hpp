#pragma once
#include "../symbolic-exec/Solver.hpp"
namespace wetstone::verification {
enum class ProofStatus{Proved,Refuted,Unknown};
struct ProofObligation {std::string id;symbolic::PathCondition assumptions;symbolic::Expr conclusion;};
inline ProofStatus discharge(const ProofObligation& obligation,symbolic::Solver& solver){
    if(obligation.conclusion.sort!=symbolic::Sort::Boolean)throw std::invalid_argument("non-Boolean conclusion");
    auto conditions=obligation.assumptions;
    conditions.push_back(symbolic::logical_not(obligation.conclusion));
    auto result=solver.check(conditions);
    if(result.status==symbolic::CheckResult::Unsat)return ProofStatus::Proved;
    if(result.status==symbolic::CheckResult::Sat)return ProofStatus::Refuted;
    return ProofStatus::Unknown;
}
}
