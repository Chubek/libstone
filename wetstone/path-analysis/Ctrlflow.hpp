#pragma once
#include "../utils/Graph.hpp"
namespace wetstone::path {
inline std::set<ir::BlockId> reachable(const ir::Function& f){return graph::reachable(f);}
inline bool can_reach(const ir::Function& f,ir::BlockId from,ir::BlockId to){
    if(!f.contains(from)||!f.contains(to))return false;
    std::set<ir::BlockId> seen{from};std::vector<ir::BlockId> stack{from};
    while(!stack.empty()){auto b=stack.back();stack.pop_back();if(b==to)return true;
        for(auto s:f.successors(b))if(seen.insert(s).second)stack.push_back(s);}
    return false;
}
}
