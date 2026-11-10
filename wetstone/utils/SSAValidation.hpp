#pragma once
#include "Graph.hpp"
#include <sstream>
namespace wetstone::ir {
inline void verify_ssa(const Function& f){
    f.validate();auto dom=graph::dominators(f);
    std::map<ValueId,std::pair<BlockId,std::size_t>> defs;
    for(auto p:f.parameters)defs[p]={invalid_block,0};
    for(const auto& b:f.blocks)for(std::size_t n=0;n<b.instructions.size();++n)
        if(b.instructions[n].result)defs[*b.instructions[n].result]={b.id,n};
    for(const auto& b:f.blocks)for(std::size_t n=0;n<b.instructions.size();++n){
        const auto& i=b.instructions[n];
        for(std::size_t k=0;k<i.arguments.size();++k){auto o=i.arguments[k];if(o.is_constant)continue;
            auto d=defs.find(o.value);
            if(d==defs.end())throw std::invalid_argument("SSA use without definition");
            auto [defblock,pos]=d->second;
            if(defblock==invalid_block)continue;
            BlockId useblock=b.id;std::size_t usepos=n;
            if(i.opcode==Opcode::Phi){useblock=i.phi_predecessors[k];usepos=f.block(useblock).instructions.size();}
            if(!dom.count(useblock))continue; // an unreachable use cannot invalidate reachable SSA
            if(!dom.at(useblock).count(defblock)||(defblock==useblock&&pos>=usepos))
                throw std::invalid_argument("SSA definition does not dominate use");
        }
    }
}
}
