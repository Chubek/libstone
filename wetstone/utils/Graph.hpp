#pragma once
#include "IR.hpp"
#include <functional>
#include <queue>

namespace wetstone::graph {
using ir::BlockId;
inline std::set<BlockId> reachable(const ir::Function& f) {
    if(!f.contains(f.entry))return {};
    std::set<BlockId> seen{f.entry};std::vector<BlockId> pending{f.entry};
    while(!pending.empty()) {auto b=pending.back();pending.pop_back();for(auto s:f.successors(b))
        if(seen.insert(s).second)pending.push_back(s);}
    return seen;
}
inline std::vector<BlockId> reverse_postorder(const ir::Function& f) {
    std::set<BlockId> seen;std::vector<BlockId> post;
    std::function<void(BlockId)> dfs=[&](BlockId b){
        if(!seen.insert(b).second)return;
        for(auto s:f.successors(b))dfs(s);
        post.push_back(b);
    };if(f.contains(f.entry))dfs(f.entry);
    std::reverse(post.begin(),post.end());return post;
}
using Dominators=std::map<BlockId,std::set<BlockId>>;
inline Dominators dominators(const ir::Function& f) {
    auto reach=reachable(f);Dominators d;
    for(auto b:reach)d[b]=(b==f.entry)?std::set<BlockId>{b}:reach;
    bool changed=true;
    while(changed) {
        changed=false;
        for(auto b:reach) {
            if(b==f.entry)continue;
            std::set<BlockId> next;bool first=true;
            for(auto p:f.predecessors(b))if(reach.count(p)) {
                if(first){next=d[p];first=false;}
                else {std::set<BlockId> intersection;std::set_intersection(next.begin(),next.end(),
                    d[p].begin(),d[p].end(),std::inserter(intersection,intersection.begin()));next.swap(intersection);}
            }
            next.insert(b);
            if(next!=d[b]){d[b]=std::move(next);changed=true;}
        }
    }return d;
}
inline std::map<BlockId,std::optional<BlockId>> immediate_dominators(const ir::Function& f) {
    const auto d=dominators(f);std::map<BlockId,std::optional<BlockId>> result;
    for(const auto& [b,ds]:d) {
        if(b==f.entry){result[b]=std::nullopt;continue;}
        std::set<BlockId> strict=ds;strict.erase(b);
        for(auto candidate:strict) {
            bool deepest=true;
            for(auto other:strict)if(other!=candidate && !d.at(candidate).count(other)){
                deepest=false;break;
            }
            if(deepest){result[b]=candidate;break;}
        }
    }return result;
}
inline std::map<BlockId,std::set<BlockId>> dominance_frontiers(const ir::Function& f) {
    auto d=dominators(f);std::map<BlockId,std::set<BlockId>> out;
    for(auto [b,_]:d)out[b];
    for(const auto& [x,_]:d)for(const auto& [y,_2]:d)
        for(auto p:f.predecessors(y))if(d.count(p) && d.at(p).count(x) &&
           (x==y||!d.at(y).count(x))) out[x].insert(y);
    return out;
}
inline std::vector<std::set<BlockId>> natural_loops(const ir::Function& f) {
    auto d=dominators(f);std::vector<std::set<BlockId>> loops;
    for(const auto& b:f.blocks)for(auto h:f.successors(b.id))if(d.count(b.id)&&d.at(b.id).count(h)) {
        std::set<BlockId> loop{h,b.id};std::vector<BlockId> work;
        if(b.id!=h)work.push_back(b.id);
        while(!work.empty()){auto n=work.back();work.pop_back();for(auto p:f.predecessors(n))
            if(loop.insert(p).second&&p!=h)work.push_back(p);}
        loops.push_back(std::move(loop));
    }return loops;
}
inline std::vector<std::set<BlockId>> strongly_connected_components(const ir::Function& f) {
    std::map<BlockId,int> index,low;std::vector<BlockId> stack;std::set<BlockId> active;
    std::vector<std::set<BlockId>> result;int next=0;
    std::function<void(BlockId)> dfs=[&](BlockId v){
        index[v]=low[v]=next++;stack.push_back(v);active.insert(v);
        for(auto w:f.successors(v))if(!index.count(w)){dfs(w);low[v]=std::min(low[v],low[w]);}
            else if(active.count(w))low[v]=std::min(low[v],index[w]);
        if(low[v]==index[v]){std::set<BlockId> comp;BlockId w;
            do{w=stack.back();stack.pop_back();active.erase(w);comp.insert(w);}while(w!=v);
            result.push_back(std::move(comp));}
    };for(const auto& b:f.blocks)if(!index.count(b.id))dfs(b.id);
    return result;
}
} // namespace wetstone::graph
