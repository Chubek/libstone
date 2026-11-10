#pragma once
#include "../utils/Graph.hpp"
#include <map>
#include <set>
namespace wetstone::df {
struct AssignmentResult {std::map<ir::BlockId,std::set<ir::ValueId>> in,out;};
inline AssignmentResult definite_assignment(const ir::Function& f) {
    f.validate();auto reach=graph::reachable(f);std::set<ir::ValueId> universe(f.parameters.begin(),f.parameters.end());
    for(const auto& b:f.blocks)for(const auto& i:b.instructions)if(i.result)universe.insert(*i.result);
    AssignmentResult r;for(auto b:reach){r.in[b]=b==f.entry?std::set<ir::ValueId>(f.parameters.begin(),f.parameters.end()):universe;r.out[b]=universe;}
    bool changed=true;while(changed){changed=false;for(auto b:graph::reverse_postorder(f)){
        std::set<ir::ValueId> input=universe;
        if(b==f.entry)input={f.parameters.begin(),f.parameters.end()};
        else {bool first=true;for(auto p:f.predecessors(b))if(reach.count(p)){
            if(first){input=r.out[p];first=false;}else{std::set<ir::ValueId> v;
                std::set_intersection(input.begin(),input.end(),r.out[p].begin(),r.out[p].end(),std::inserter(v,v.end()));input.swap(v);}
            }}
        auto output=input;for(const auto& i:f.block(b).instructions)if(i.result)output.insert(*i.result);
        if(input!=r.in[b]||output!=r.out[b]){r.in[b]=input;r.out[b]=output;changed=true;}
    }}return r;
}
}
