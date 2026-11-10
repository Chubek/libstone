#pragma once
#include "Forwards.hpp"
#include "SetLattice.hpp"
namespace wetstone::df {
using Definition=std::pair<ir::BlockId,std::size_t>;
using DefSet=MaySet<Definition>;
inline Result<DefSet> reaching_definitions(const ir::Function& f,std::size_t limit=100000){
    auto transfer=[&](ir::BlockId b,DefSet s){const auto& ins=f.block(b).instructions;
        for(std::size_t i=0;i<ins.size();++i)if(ins[i].result)s.values.insert({b,i});return s;};
    return forwards<DefSet>(f,transfer,DefSet::bottom(),limit);
}
}
