#pragma once
#include "Ctrlflow.hpp"
#include <map>
#include <vector>
namespace wetstone::path {
struct Enumeration {
    std::vector<std::vector<ir::BlockId>> paths;
    bool truncated=false;std::size_t explored=0;
};
inline Enumeration enumerate(const ir::Function& f,std::size_t max_paths=128,std::size_t max_depth=64,
                              std::size_t max_visits_per_block=2){
    f.validate();Enumeration r;std::vector<ir::BlockId> trail;
    std::map<ir::BlockId,std::size_t> visits;
    std::function<void(ir::BlockId)> dfs=[&](ir::BlockId id){
        if(r.paths.size()>=max_paths||trail.size()>=max_depth||visits[id]>=max_visits_per_block){r.truncated=true;return;}
        ++r.explored;++visits[id];trail.push_back(id);
        auto next=f.successors(id);
        if(next.empty())r.paths.push_back(trail);
        else for(auto b:next)dfs(b);
        trail.pop_back();--visits[id];
    };dfs(f.entry);return r;
}
}
