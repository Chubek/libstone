#pragma once
#include "CallGraph.hpp"
#include <functional>
namespace wetstone::interprocedural {
inline std::vector<std::set<std::string>> recursive_components(const CallGraph& graph){
    std::map<std::string,int> idx,low;std::vector<std::string> stack;std::set<std::string> active;
    std::vector<std::set<std::string>> components;int count=0;
    std::function<void(const std::string&)> dfs=[&](const std::string& f){
        idx[f]=low[f]=count++;stack.push_back(f);active.insert(f);
        for(const auto& g:graph.callees(f))if(graph.edges.count(g)){
            if(!idx.count(g)){dfs(g);low[f]=std::min(low[f],low[g]);}
            else if(active.count(g))low[f]=std::min(low[f],idx[g]);
        }
        if(idx[f]==low[f]){std::set<std::string> comp;std::string v;
            do{v=stack.back();stack.pop_back();active.erase(v);comp.insert(v);}while(v!=f);
            components.push_back(std::move(comp));}
    };for(const auto& [f,_]:graph.edges)if(!idx.count(f))dfs(f);
    return components;
}
}
