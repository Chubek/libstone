#pragma once
#include "../dataflow-analysis/Forwards.hpp"
#include "../dataflow-analysis/SetLattice.hpp"
namespace wetstone::path {
using TaintSet=df::MaySet<ir::ValueId>;
inline df::Result<TaintSet> taint(const ir::Function& f,std::size_t limit=100000){
    auto step=[&](ir::BlockId b,TaintSet state){
        for(const auto& i:f.block(b).instructions)if(i.result){
            bool bad=i.opcode==ir::Opcode::Input;
            if(i.opcode!=ir::Opcode::Sanitize)for(const auto& a:i.arguments)
                if(!a.is_constant&&state.values.count(a.value))bad=true;
            if(bad)state.values.insert(*i.result);else state.values.erase(*i.result);
        }return state;
    };return df::forwards<TaintSet>(f,step,TaintSet::bottom(),limit);
}
}
