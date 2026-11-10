#pragma once
#include "../abstract-interp/Transfer.hpp"
#include "Path.hpp"
#include <queue>
namespace wetstone::path {
struct PathState {ir::BlockId block;ai::Environment environment;std::vector<ir::BlockId> trail;};
struct PathResult {std::vector<PathState> exits;std::size_t explored=0;bool truncated=false;};
inline PathResult analyze_paths(const ir::Function& f,std::size_t state_budget=1000,std::size_t depth_limit=32,
                               std::size_t visits_per_block=2){
    f.validate();PathResult r;
    std::queue<PathState> pending;pending.push({f.entry,ai::Environment::top(),{}});
    while(!pending.empty()){
        if(r.explored>=state_budget){r.truncated=true;break;}
        auto s=std::move(pending.front());pending.pop();++r.explored;
        if(s.trail.size()>=depth_limit || std::count(s.trail.begin(),s.trail.end(),s.block)>=static_cast<std::ptrdiff_t>(visits_per_block)){
            r.truncated=true;continue;
        }
        s.trail.push_back(s.block);
        const auto& b=f.block(s.block);
        s.environment=ai::transfer_block(b,s.environment);
        if(s.environment.is_bottom())continue;
        auto successors=f.successors(s.block);
        if(successors.empty()){r.exits.push_back(std::move(s));continue;}
        const auto& tail=b.instructions.back();
        if(tail.opcode==ir::Opcode::Branch){auto cond=ai::operand(s.environment,tail.arguments.at(0));
            if(cond.lo==cond.hi){pending.push({successors[cond.lo!=0?0:1],s.environment,s.trail});continue;}
        }
        for(auto next:successors)pending.push({next,s.environment,s.trail});
    }return r;
}
}
