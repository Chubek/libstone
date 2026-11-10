#pragma once
#include "Sink.hpp"
#include "../symbolic-exec/Solver.hpp"
namespace wetstone::repair {
struct RepairChoice {std::string id;std::int64_t cost=1;};
struct RepairPlan {std::vector<std::string> selected;bool proved_feasible=false;bool inconclusive=false;};
// Searches minimal-cost Boolean repair selections using Satie. Each conflict
// requires >=1 repair option; at-most-one may be expressed with explicit clauses.
inline RepairPlan choose_repairs(const std::vector<RepairChoice>& options,
                   const std::vector<std::vector<std::size_t>>& conflicts,
                   std::size_t search_limit=10000){
    RepairPlan best;best.inconclusive=true;
    if(options.empty())return best;
    for(auto& op:options)if(op.cost<0)throw std::invalid_argument("negative repair cost");
    if(options.size()>20||options.size()>search_limit)return best;
    symbolic::SatieSolver solver;
    std::int64_t best_cost=INT64_MAX;
    const std::size_t n=options.size();
    // Exponential candidate enumeration bounded by caller budget; SAT validates each
    // candidate against conflicts, rather than hard-coding a separate SAT solver.
    std::size_t evaluated=0;
    for(std::uint64_t mask=0;mask<(1ULL<<n)&&evaluated<search_limit;++mask){
        std::int64_t sum=0;for(std::size_t i=0;i<n;++i)if(mask&(1ULL<<i)){
            if(options[i].cost>INT64_MAX-sum){sum=INT64_MAX;break;}sum+=options[i].cost;
        }
        if(sum>=best_cost)continue;
        symbolic::PathCondition clauses;
        for(std::size_t i=0;i<n;++i){auto v=symbolic::Expr::variable("repair_"+std::to_string(i),symbolic::Sort::Boolean);
            clauses.push_back(mask&(1ULL<<i)?v:symbolic::logical_not(v));}
        for(const auto& conflict:conflicts){
            std::optional<symbolic::Expr> expr;
            for(auto idx:conflict){if(idx>=n)throw std::out_of_range("repair index");
                auto v=symbolic::Expr::variable("repair_"+std::to_string(idx),symbolic::Sort::Boolean);
                expr=expr?symbolic::Expr::binary(symbolic::Kind::Or,*expr,v):v;
            }clauses.push_back(expr.value_or(symbolic::Expr::boolean(false)));
        }
        ++evaluated;auto r=solver.check(clauses);
        if(r.status==symbolic::CheckResult::Unknown)return best;
        if(r.status==symbolic::CheckResult::Sat){best_cost=sum;best.selected.clear();
            for(std::size_t i=0;i<n;++i)if(mask&(1ULL<<i))best.selected.push_back(options[i].id);
            best.proved_feasible=true;best.inconclusive=false;
        }
    }
    if(evaluated>=search_limit)best.inconclusive=true;
    return best;
}
}
