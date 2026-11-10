#pragma once
#include "../utils/Graph.hpp"
#include <map>
#include <set>
namespace wetstone::df {
using ExpressionKey=std::pair<ir::Opcode,std::vector<ir::Operand>>;
inline std::map<ir::BlockId,std::set<ExpressionKey>> available_expressions(const ir::Function& f){
    f.validate();auto reach=graph::reachable(f);std::set<ExpressionKey> universe;
    for(const auto& b:f.blocks)for(const auto& i:b.instructions)if(i.result&&i.arguments.size()==2&&i.opcode!=ir::Opcode::Phi&&i.opcode!=ir::Opcode::Load&&i.opcode!=ir::Opcode::Call)
        universe.insert({i.opcode,i.arguments});
    std::map<ir::BlockId,std::set<ExpressionKey>> in,out;
    for(auto b:reach){in[b]=b==f.entry?std::set<ExpressionKey>{}:universe;out[b]=universe;}
    bool change=true;while(change){change=false;for(auto b:graph::reverse_postorder(f)){
        std::set<ExpressionKey> next=universe;
        if(b==f.entry)next.clear();else{bool first=true;for(auto p:f.predecessors(b))if(reach.count(p)){
            if(first){next=out[p];first=false;}else{std::set<ExpressionKey> t;
                std::set_intersection(next.begin(),next.end(),out[p].begin(),out[p].end(),std::inserter(t,t.end()));next.swap(t);}
        }}
        auto s=next;
        for(const auto& i:f.block(b).instructions){
            if(i.result)for(auto it=s.begin();it!=s.end();) {
                bool uses=false;for(auto a:it->second)if(!a.is_constant&&a.value==*i.result)uses=true;
                if(uses)it=s.erase(it);else ++it;
            }
            if(i.result&&i.arguments.size()==2&&i.opcode!=ir::Opcode::Phi&&i.opcode!=ir::Opcode::Load&&i.opcode!=ir::Opcode::Call)s.insert({i.opcode,i.arguments});
        }
        if(in[b]!=next||out[b]!=s){in[b]=std::move(next);out[b]=std::move(s);change=true;}
    }}return in;
}
}
