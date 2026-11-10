#pragma once
#include "Ctrlflow.hpp"
namespace wetstone::path {
// Exit-conditioned postdominance: blocks without paths to any exit are excluded.
inline graph::Dominators postdominators(const ir::Function& f){
    std::set<ir::BlockId> exits,can_exit;
    for(const auto& b:f.blocks)if(f.successors(b.id).empty())exits.insert(b.id);
    std::vector<ir::BlockId> work(exits.begin(),exits.end());can_exit=exits;
    while(!work.empty()){auto b=work.back();work.pop_back();for(auto p:f.predecessors(b))
        if(can_exit.insert(p).second)work.push_back(p);}
    graph::Dominators d;for(auto b:can_exit)d[b]=exits.count(b)?std::set<ir::BlockId>{b}:can_exit;
    bool changed=true;while(changed){changed=false;for(auto b:can_exit)if(!exits.count(b)){
        auto next=can_exit;bool first=true;
        for(auto s:f.successors(b))if(can_exit.count(s)){
            if(first){next=d[s];first=false;}else{std::set<ir::BlockId> tmp;
                std::set_intersection(next.begin(),next.end(),d[s].begin(),d[s].end(),std::inserter(tmp,tmp.end()));next.swap(tmp);}
        }next.insert(b);if(next!=d[b]){d[b]=std::move(next);changed=true;}
    }}return d;
}
}
