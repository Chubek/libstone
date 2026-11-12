#pragma once
#if __has_include("../../../core/Core.hpp")
#include "../../../core/Core.hpp"
#else
#include <moonstone/core/Core.hpp>
#endif
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_set>
namespace moonstone::supersonic {
// Typed, bounded enumerative synthesizer for pure integer expressions.
enum class Op { Constant, Variable, Add, Sub, Mul, Neg };
struct Expr {Op op=Op::Constant;std::int64_t value=0;std::string variable;std::vector<Expr> args;
 std::string print()const{switch(op){case Op::Constant:return std::to_string(value);case Op::Variable:return variable;case Op::Neg:return "(-"+args.at(0).print()+")";default:{const char* opn=op==Op::Add?"+":op==Op::Sub?"-":"*";return "("+args.at(0).print()+opn+args.at(1).print()+")";}}}
};
inline std::optional<std::int64_t> checked(Op op,std::int64_t a,std::int64_t b=0){
 std::int64_t out=0;bool overflow=false;switch(op){case Op::Add:overflow=__builtin_add_overflow(a,b,&out);break;case Op::Sub:overflow=__builtin_sub_overflow(a,b,&out);break;case Op::Mul:overflow=__builtin_mul_overflow(a,b,&out);break;case Op::Neg:overflow=__builtin_sub_overflow(std::int64_t(0),a,&out);break;default:return std::nullopt;}return overflow?std::nullopt:std::optional<std::int64_t>(out);
}
inline std::optional<std::int64_t> evaluate(const Expr& e,const std::map<std::string,std::int64_t>& vars){
 if(e.op==Op::Constant)return e.value;if(e.op==Op::Variable){auto it=vars.find(e.variable);return it==vars.end()?std::nullopt:std::optional<std::int64_t>(it->second);}
 if(e.args.size()!=(e.op==Op::Neg?1u:2u))return std::nullopt;auto a=evaluate(e.args[0],vars);if(!a)return std::nullopt;if(e.op==Op::Neg)return checked(e.op,*a);auto b=evaluate(e.args[1],vars);if(!b)return std::nullopt;return checked(e.op,*a,*b);
}
struct Example {std::map<std::string,std::int64_t> inputs;std::int64_t expected=0;};
struct Grammar {std::vector<std::string> variables;std::vector<std::int64_t> constants;std::vector<Op> operations{Op::Add,Op::Sub,Op::Mul};};
struct Budget {std::size_t max_cost=6,max_candidates=100000;};
struct Problem {Grammar grammar;std::vector<Example> examples;Budget budget;std::function<bool(const Expr&)> predicate;};
enum class SolveStatus { Found, UnsatisfiableWithinBound, BudgetExhausted, InvalidInput };
struct Solution {SolveStatus status=SolveStatus::InvalidInput;std::optional<Expr> program;std::size_t candidates=0;};
inline Solution solve(const Problem& p){
 if(p.examples.empty()||p.budget.max_cost==0||p.budget.max_cost>12||p.budget.max_candidates==0)return {};
 Solution result;std::vector<std::vector<Expr>> level(p.budget.max_cost+1);std::unordered_set<std::string> seen;
 auto test=[&](const Expr& e)->bool{if(++result.candidates>p.budget.max_candidates)return false;for(auto& ex:p.examples){auto v=evaluate(e,ex.inputs);if(!v||*v!=ex.expected)return false;}return !p.predicate||p.predicate(e);};
 auto add=[&](std::size_t cost,Expr e)->bool{auto repr=e.print();if(!seen.insert(repr).second)return false;level[cost].push_back(std::move(e));return test(level[cost].back());};
 for(auto c:p.grammar.constants){Expr e;e.value=c;if(add(1,std::move(e))){result.status=SolveStatus::Found;result.program=level[1].back();return result;}if(result.candidates>p.budget.max_candidates)break;}
 if(result.candidates<=p.budget.max_candidates)for(auto& v:p.grammar.variables){if(v.empty())continue;Expr e;e.op=Op::Variable;e.variable=v;if(add(1,std::move(e))){result.status=SolveStatus::Found;result.program=level[1].back();return result;}if(result.candidates>p.budget.max_candidates)break;}
 for(std::size_t cost=2;cost<=p.budget.max_cost&&result.candidates<=p.budget.max_candidates;++cost){
  for(auto op:p.grammar.operations){if(op==Op::Neg){for(auto& a:level[cost-1]){Expr e{op,0,{}, {a}};if(add(cost,std::move(e))){result.status=SolveStatus::Found;result.program=level[cost].back();return result;}if(result.candidates>p.budget.max_candidates)break;}continue;}
   if(op!=Op::Add&&op!=Op::Sub&&op!=Op::Mul)continue;
   for(std::size_t left=1;left+1<cost&&result.candidates<=p.budget.max_candidates;++left){auto right=cost-left-1;for(auto& a:level[left]){for(auto& b:level[right]){
    if((op==Op::Add||op==Op::Mul)&&a.print()>b.print())continue;
    Expr e{op,0,{}, {a,b}};if(add(cost,std::move(e))){result.status=SolveStatus::Found;result.program=level[cost].back();return result;}if(result.candidates>p.budget.max_candidates)break;
   }if(result.candidates>p.budget.max_candidates)break;}}
  }
 }
 result.status=result.candidates>p.budget.max_candidates?SolveStatus::BudgetExhausted:SolveStatus::UnsatisfiableWithinBound;return result;
}
}
