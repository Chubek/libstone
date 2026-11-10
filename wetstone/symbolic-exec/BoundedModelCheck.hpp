#pragma once
#include "SymbolicExec.hpp"
namespace wetstone::symbolic {
struct BMCResult {bool has_counterexample=false;bool inconclusive=false;std::size_t paths_checked=0;};
inline BMCResult check_assertions(const ir::Function& f,Solver& solver,std::size_t budget=1000){
    // Treat each assert as an obligation under symbolic path conditions.
    // Bounded execution itself is explicit; assertions are evaluated per path
    // by conservatively converting them to negated path obligations.
    auto run=execute(f,&solver,budget);BMCResult out;out.inconclusive=run.truncated;
    for(const auto& state:run.completed){++out.paths_checked;
        for(auto b:state.trace)for(const auto& i:f.block(b).instructions)if(i.opcode==ir::Opcode::Assert&&!i.arguments.empty()){
            auto path=state.constraints;path.push_back(logical_not(as_boolean(operand(state,i.arguments[0]))));
            auto answer=solver.check(path);if(answer.status==CheckResult::Sat)out.has_counterexample=true;
            if(answer.status==CheckResult::Unknown)out.inconclusive=true;
        }
    }return out;
}
}
