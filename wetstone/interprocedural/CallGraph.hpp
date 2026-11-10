#pragma once
#include "../utils/Program.hpp"
namespace wetstone::interprocedural {
struct CallGraph {
    std::map<std::string,std::set<std::string>> edges;
    std::set<std::string> unresolved;
    std::vector<std::string> callees(const std::string& caller)const{
        auto it=edges.find(caller);return it==edges.end()?std::vector<std::string>{}:std::vector<std::string>(it->second.begin(),it->second.end());
    }
};
inline CallGraph call_graph(const ir::Program& program){
    CallGraph r;for(const auto& f:program.functions)r.edges[f.name];
    for(const auto& f:program.functions)for(const auto& b:f.blocks)for(const auto& op:b.instructions)
        if(op.opcode==ir::Opcode::Call){r.edges[f.name].insert(op.symbol);
            if(op.symbol.empty()||!program.lookup(op.symbol))r.unresolved.insert(op.symbol);}
    return r;
}
}
