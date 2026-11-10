#pragma once
#include "Backwards.hpp"
#include "SetLattice.hpp"
namespace wetstone::df {
using LiveSet=MaySet<ir::ValueId>;
inline Result<LiveSet> liveness(const ir::Function& f,std::size_t limit=100000) {
    auto transfer=[&](ir::BlockId b,LiveSet s){const auto& ins=f.block(b).instructions;
        for(auto it=ins.rbegin();it!=ins.rend();++it){
            if(it->result)s.values.erase(*it->result);
            for(const auto& a:it->arguments)if(!a.is_constant)s.values.insert(a.value);
        }return s;
    };return backwards<LiveSet>(f,transfer,{},limit);
}
}
