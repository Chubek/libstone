#pragma once
#include <algorithm>
#include <compare>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wetstone::ir {
using ValueId=std::uint32_t;
using BlockId=std::uint32_t;
inline constexpr ValueId invalid_value=std::numeric_limits<ValueId>::max();
inline constexpr BlockId invalid_block=std::numeric_limits<BlockId>::max();

struct Operand {
    bool is_constant=false;
    std::int64_t literal=0;
    ValueId value=invalid_value;
    static Operand constant(std::int64_t x) { return {true,x,invalid_value}; }
    static Operand ref(ValueId id) { if(id==invalid_value)throw std::invalid_argument("invalid SSA id"); return {false,0,id}; }
    friend auto operator<=>(const Operand&,const Operand&)=default;
};
enum class Opcode {
    Nop, Constant, Copy, Add, Sub, Mul, Div, Mod, Neg, BitAnd, BitOr, BitXor,
    Shl, Shr, Eq, Ne, Lt, Le, Gt, Ge, Phi, Alloc, AddressOf, Load, Store,
    Free, Call, Return, Jump, Branch, Assert, Assume, Input, Sanitize
};
inline const char* name(Opcode op) {
    switch(op) {
#define WETSTONE_OP(n) case Opcode::n: return #n;
    WETSTONE_OP(Nop) WETSTONE_OP(Constant) WETSTONE_OP(Copy) WETSTONE_OP(Add)
    WETSTONE_OP(Sub) WETSTONE_OP(Mul) WETSTONE_OP(Div) WETSTONE_OP(Mod)
    WETSTONE_OP(Neg) WETSTONE_OP(BitAnd) WETSTONE_OP(BitOr) WETSTONE_OP(BitXor)
    WETSTONE_OP(Shl) WETSTONE_OP(Shr) WETSTONE_OP(Eq) WETSTONE_OP(Ne)
    WETSTONE_OP(Lt) WETSTONE_OP(Le) WETSTONE_OP(Gt) WETSTONE_OP(Ge)
    WETSTONE_OP(Phi) WETSTONE_OP(Alloc) WETSTONE_OP(AddressOf) WETSTONE_OP(Load)
    WETSTONE_OP(Store) WETSTONE_OP(Free) WETSTONE_OP(Call) WETSTONE_OP(Return)
    WETSTONE_OP(Jump) WETSTONE_OP(Branch) WETSTONE_OP(Assert) WETSTONE_OP(Assume)
    WETSTONE_OP(Input) WETSTONE_OP(Sanitize)
#undef WETSTONE_OP
    } return "Unknown";
}
struct Instruction {
    Opcode opcode=Opcode::Nop;
    std::optional<ValueId> result;
    std::vector<Operand> arguments;
    std::vector<BlockId> targets;
    std::vector<BlockId> phi_predecessors;
    std::string symbol; // for alloc, global object, call target or source label
    unsigned source_line=0;
};
struct Block {
    BlockId id=invalid_block;
    std::vector<Instruction> instructions;
};
struct Function {
    std::string name;
    BlockId entry=invalid_block;
    std::vector<ValueId> parameters;
    std::vector<Block> blocks;
    const Block& block(BlockId id) const {
        for(const auto& b:blocks)if(b.id==id)return b;
        throw std::out_of_range("unknown block");
    }
    Block& block(BlockId id) {
        for(auto& b:blocks)if(b.id==id)return b;
        throw std::out_of_range("unknown block");
    }
    bool contains(BlockId id) const {
        return std::any_of(blocks.begin(),blocks.end(),[&](const Block& b){return b.id==id;});
    }
    std::vector<BlockId> successors(BlockId id) const {
        const auto& b=block(id);
        if(b.instructions.empty()) return {};
        const auto& t=b.instructions.back();
        return (t.opcode==Opcode::Jump||t.opcode==Opcode::Branch)?t.targets:std::vector<BlockId>{};
    }
    std::vector<BlockId> predecessors(BlockId id) const {
        block(id);
        std::vector<BlockId> result;
        for(const auto& b:blocks)for(auto s:successors(b.id))if(s==id){result.push_back(b.id);break;}
        return result;
    }
    void validate() const {
        if(!contains(entry))throw std::invalid_argument("function entry not found");
        std::set<BlockId> ids;
        std::set<ValueId> defs;
        for(auto p:parameters)if(p==invalid_value||!defs.insert(p).second)
            throw std::invalid_argument("duplicate or invalid parameter");
        for(const auto& b:blocks) {
            if(b.id==invalid_block||!ids.insert(b.id).second) throw std::invalid_argument("duplicate or invalid block id");
            for(std::size_t n=0;n<b.instructions.size();++n) {
                const auto& i=b.instructions[n];
                if(i.result && (*i.result==invalid_value||!defs.insert(*i.result).second))
                    throw std::invalid_argument("SSA id defined more than once");
                bool term=i.opcode==Opcode::Jump||i.opcode==Opcode::Branch||i.opcode==Opcode::Return;
                if(term && n+1!=b.instructions.size())throw std::invalid_argument("terminator must be last");
                if(i.opcode==Opcode::Branch && (i.targets.size()!=2||i.arguments.size()!=1))
                    throw std::invalid_argument("branch requires condition and two targets");
                if(i.opcode==Opcode::Jump && i.targets.size()!=1)throw std::invalid_argument("jump requires one target");
                if(i.opcode==Opcode::Phi && i.phi_predecessors.size()!=i.arguments.size())
                    throw std::invalid_argument("phi input/edge count mismatch");
            }
        }
        for(const auto& b:blocks)for(auto s:successors(b.id))if(!contains(s))
            throw std::invalid_argument("edge targets absent block");
        for(const auto& b:blocks)for(const auto& i:b.instructions)if(i.opcode==Opcode::Phi){
            const auto p=predecessors(b.id);
            if(std::set<BlockId>(p.begin(),p.end())!=std::set<BlockId>(i.phi_predecessors.begin(),i.phi_predecessors.end())
               ||std::set<BlockId>(i.phi_predecessors.begin(),i.phi_predecessors.end()).size()!=i.phi_predecessors.size())
                throw std::invalid_argument("phi incoming edges must match CFG predecessors");
        }
    }
};
} // namespace wetstone::ir
