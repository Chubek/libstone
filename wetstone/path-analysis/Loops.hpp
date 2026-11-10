#pragma once
#include "Dominance.hpp"
namespace wetstone::path {
struct Loop {ir::BlockId header;std::set<ir::BlockId> body;std::set<ir::BlockId> exits;};
inline std::vector<Loop> natural_loops(const ir::Function& f){
    auto dom=graph::dominators(f);std::vector<Loop> result;
    for(const auto& b:f.blocks)for(auto h:f.successors(b.id))if(dom.count(b.id)&&dom.at(b.id).count(h)){
        Loop loop{h,{h,b.id},{}};std::vector<ir::BlockId> work;if(b.id!=h)work.push_back(b.id);
        while(!work.empty()){auto v=work.back();work.pop_back();for(auto p:f.predecessors(v))
            if(loop.body.insert(p).second&&p!=h)work.push_back(p);}
        for(auto v:loop.body)for(auto s:f.successors(v))if(!loop.body.count(s))loop.exits.insert(s);
        result.push_back(std::move(loop));
    }return result;
}
}
