#pragma once
#include "../utils/IR.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>
#if __has_include(<Flintstone.hpp>)
#include <Flintstone.hpp>
#define WETSTONE_HAS_FLINTSTONE 1
#else
#define WETSTONE_HAS_FLINTSTONE 0
#endif
namespace wetstone::integration {
inline constexpr bool flintstone_available=WETSTONE_HAS_FLINTSTONE;
#if WETSTONE_HAS_FLINTSTONE
// Flintstone's built-in arith dialect uses explicit modular integers.
// Unless mathematical_integer_semantics is opted into, modular arithmetic is
// deliberately abstracted to Input/Top, not analyzed as unbounded Z.
inline ir::Opcode classify(const std::string& name){
    using Op=ir::Opcode;
    if(name=="arith.constant")return Op::Constant;if(name=="arith.copy")return Op::Copy;
    if(name=="arith.add")return Op::Add;if(name=="arith.sub")return Op::Sub;
    if(name=="arith.mul")return Op::Mul;if(name=="arith.div")return Op::Div;
    if(name=="arith.mod")return Op::Mod;if(name=="arith.neg")return Op::Neg;
    if(name=="arith.and")return Op::BitAnd;if(name=="arith.or")return Op::BitOr;
    if(name=="arith.xor")return Op::BitXor;if(name=="arith.shl")return Op::Shl;
    if(name=="arith.shr")return Op::Shr;
    if(name=="arith.eq")return Op::Eq;if(name=="arith.ne")return Op::Ne;
    if(name=="arith.lt")return Op::Lt;if(name=="arith.le")return Op::Le;
    if(name=="arith.gt")return Op::Gt;if(name=="arith.ge")return Op::Ge;
    if(name=="cf.phi")return Op::Phi;if(name=="cf.jump")return Op::Jump;if(name=="cf.branch")return Op::Branch;
    if(name=="func.call")return Op::Call;if(name=="func.return")return Op::Return;
    if(name=="mem.load")return Op::Load;if(name=="mem.store")return Op::Store;
    return Op::Nop;
}
inline ir::Function convert(const flintstone::Function& function,bool mathematical_integer_semantics=false){
    ir::Function f;f.name=function.name;f.entry=function.entry;
    for(const auto& p:function.parameters)f.parameters.push_back(p.id);
    for(const auto& block:function.blocks){ir::Block b;b.id=block.id;
        for(const auto& original:block.operations){
            if(original.results.size()>1)throw std::invalid_argument("multi-result op needs custom adapter");
            ir::Instruction i;i.opcode=classify(original.name);i.source_line=static_cast<unsigned>(original.location.line);
            if(!original.results.empty())i.result=original.results[0].id;
            for(const auto& a:original.operands){
                switch(a.kind){
                    case flintstone::OperandKind::Value:i.arguments.push_back(ir::Operand::ref(a.value));break;
                    case flintstone::OperandKind::Integer:i.arguments.push_back(ir::Operand::constant(a.integer));break;
                    default:throw std::invalid_argument("unsupported operand; provide custom typed analysis");
                }
            }
            i.targets=original.successors;i.phi_predecessors=original.incoming;
            if(original.name=="func.call")if(auto it=original.attributes.find("callee");it!=original.attributes.end())
                if(auto s=std::get_if<std::string>(&it->second))i.symbol=*s;
            if(i.opcode==ir::Opcode::Nop&&i.result)i.opcode=ir::Opcode::Input;
            if(!mathematical_integer_semantics && original.name.rfind("arith.",0)==0 &&
               original.name!="arith.copy"&&original.name!="arith.constant"&&i.result)i.opcode=ir::Opcode::Input;
            b.instructions.push_back(std::move(i));
        }
        f.blocks.push_back(std::move(b));
    }f.validate();return f;
}
#endif
}
