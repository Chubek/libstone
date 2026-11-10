#pragma once
#include "../utils/IR.hpp"
#include <map>
namespace wetstone::pointer {
struct Callsite {ir::BlockId block;std::size_t instruction;std::string callee;std::vector<ir::ValueId> operands;};
inline std::vector<Callsite> calls(const ir::Function& f){
    std::vector<Callsite> out;
    for(const auto& b:f.blocks)for(std::size_t n=0;n<b.instructions.size();++n){
        const auto& i=b.instructions[n];if(i.opcode!=ir::Opcode::Call)continue;
        Callsite s{b.id,n,i.symbol,{}};for(auto o:i.arguments)if(!o.is_constant)s.operands.push_back(o.value);
        out.push_back(std::move(s));
    }return out;
}
}
