#pragma once
#include "AvailableExpressions.hpp"
namespace wetstone::df {
// Intra-block exact value-numbering for side-effect-free SSA operators.
inline std::map<ir::ValueId,ir::ValueId> redundant_expressions(const ir::Function& f){
    std::map<ir::ValueId,ir::ValueId> redundant;
    for(const auto& b:f.blocks){std::map<ExpressionKey,ir::ValueId> known;
        for(const auto& i:b.instructions){
            if(!i.result || i.arguments.empty() ||i.opcode==ir::Opcode::Phi||i.opcode==ir::Opcode::Call||
                i.opcode==ir::Opcode::Load||i.opcode==ir::Opcode::Alloc||i.opcode==ir::Opcode::Input)continue;
            ExpressionKey key{i.opcode,i.arguments};
            if(auto it=known.find(key);it!=known.end())redundant[*i.result]=it->second;
            else known[key]=*i.result;
        }
    }return redundant;
}
}
