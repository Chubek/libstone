#pragma once
#include "PostDominance.hpp"
namespace wetstone::path {
inline std::map<ir::BlockId,std::set<ir::BlockId>> control_dependencies(const ir::Function& f){
    auto post=postdominators(f);std::map<ir::BlockId,std::set<ir::BlockId>> dep;
    for(const auto& b:f.blocks)for(auto next:f.successors(b.id)){
        if(!post.count(b.id)||!post.count(next)||post.at(b.id).count(next))continue;
        for(const auto& [candidate,doms]:post){
            // Under exit-conditioned semantics, candidate lies after this branch,
            // reachable from successor, but need not postdominate branch.
            if(post.at(next).count(candidate)&&!post.at(b.id).count(candidate))dep[b.id].insert(candidate);
        }
    }return dep;
}
}
