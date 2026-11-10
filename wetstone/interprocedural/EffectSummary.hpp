#pragma once
#include "CallGraph.hpp"
#include "../pointer-analysis/ModRef.hpp"
namespace wetstone::interprocedural {
struct EffectSummary {bool reads=false,writes=false,allocates=false,frees=false,unknown_call=false;
    bool merge(const EffectSummary& other){auto old=*this;reads|=other.reads;writes|=other.writes;
        allocates|=other.allocates;frees|=other.frees;unknown_call|=other.unknown_call;
        return !(old==*this);}
    friend bool operator==(const EffectSummary&,const EffectSummary&)=default;
};
inline std::map<std::string,EffectSummary> effect_summaries(const ir::Program& program){
    auto cg=call_graph(program);std::map<std::string,EffectSummary> out;
    for(const auto& f:program.functions){auto m=pointer::effects(f);
        out[f.name]={m.reads,m.writes,m.allocates,m.frees,false};}
    bool changed=true;while(changed){changed=false;
        for(const auto& [f,callees]:cg.edges)for(const auto& callee:callees){
            if(auto it=out.find(callee);it!=out.end())changed|=out[f].merge(it->second);
            else changed|=out[f].merge({true,true,true,true,true});
        }
    }return out;
}
}
