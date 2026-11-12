#pragma once
#include "Supersonic.hpp"
namespace moonstone::supersonic {
// Oracle must return an actual counterexample, or nullopt only for a fully
// verified candidate. Otherwise callers must NOT treat Found as a proof.
using CounterexampleOracle=std::function<std::optional<Example>(const Expr&)>;
struct CegisSolution {Solution solution;std::size_t iterations=0;bool fully_verified=false;};
inline CegisSolution cegis(Problem problem,const CounterexampleOracle& oracle,std::size_t max_iterations=64){
 CegisSolution result;if(!oracle)return result;
 for(std::size_t step=0;step<max_iterations;++step){
  auto s=solve(problem);result.iterations=step+1;if(s.status!=SolveStatus::Found){result.solution=std::move(s);return result;}
  auto failing=oracle(*s.program);if(!failing){result.solution=std::move(s);result.fully_verified=true;return result;}
  bool duplicate=false;for(auto& e:problem.examples)if(e.inputs==failing->inputs){if(e.expected!=failing->expected){result.solution.status=SolveStatus::InvalidInput;return result;}duplicate=true;break;}
  if(duplicate){result.solution.status=SolveStatus::InvalidInput;return result;}
  problem.examples.push_back(std::move(*failing));
 }
 result.solution.status=SolveStatus::BudgetExhausted;return result;
}
}
