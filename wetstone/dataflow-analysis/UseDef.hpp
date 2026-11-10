#pragma once
#include "../utils/IR.hpp"
namespace wetstone::df {
struct UseLocation{ir::BlockId block;std::size_t instruction,argument;};
struct UseDef {
    std::map<ir::ValueId,std::pair<ir::BlockId,std::size_t>> definitions;
    std::map<ir::ValueId,std::vector<UseLocation>> uses;
};
inline UseDef use_def(const ir::Function& f){
    UseDef r;for(const auto& b:f.blocks)for(std::size_t n=0;n<b.instructions.size();++n){
        const auto& i=b.instructions[n];if(i.result)r.definitions[*i.result]={b.id,n};
        for(std::size_t k=0;k<i.arguments.size();++k)if(!i.arguments[k].is_constant)
            r.uses[i.arguments[k].value].push_back({b.id,n,k});
    }return r;
}
}
