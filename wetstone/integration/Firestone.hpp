#pragma once
#include "../utils/IR.hpp"
#if __has_include(<libir/SimpleSSAGraph.hpp>)
#include <libir/SimpleSSAGraph.hpp>
#define WETSTONE_HAS_FIRESTONE 1
#else
#define WETSTONE_HAS_FIRESTONE 0
#endif
namespace wetstone::integration {
inline constexpr bool firestone_available=WETSTONE_HAS_FIRESTONE;
#if WETSTONE_HAS_FIRESTONE
inline ir::Function from_firestone_cfg(const firestone::ir::ControlFlowGraph& cfg){
    ir::Function f;f.entry=cfg.entry();
    for(auto id:cfg.blocks()){
        ir::Block block;block.id=id;
        for(const auto& source:cfg.block(id).instructions){
            ir::Instruction i;
            using FO=firestone::ir::Opcode;using WO=ir::Opcode;
            switch(source.opcode){
                case FO::Constant:i.opcode=WO::Constant;break;case FO::Copy:i.opcode=WO::Copy;break;
                case FO::Add:i.opcode=WO::Add;break;case FO::Sub:i.opcode=WO::Sub;break;
                case FO::Mul:i.opcode=WO::Mul;break;case FO::Div:i.opcode=WO::Div;break;
                case FO::Phi:i.opcode=WO::Phi;break;case FO::Jump:i.opcode=WO::Jump;break;
                case FO::Branch:i.opcode=WO::Branch;break;case FO::Return:i.opcode=WO::Return;break;
                case FO::Load:i.opcode=WO::Load;break;case FO::Store:i.opcode=WO::Store;break;
                default:i.opcode=WO::Nop;break;
            }
            if(source.result)i.result=static_cast<ir::ValueId>(*source.result);
            for(const auto& a:source.operands) {
                switch(a.kind){
                    case firestone::ir::OperandKind::Value:i.arguments.push_back(ir::Operand::ref(static_cast<ir::ValueId>(a.value)));break;
                    case firestone::ir::OperandKind::Integer:i.arguments.push_back(ir::Operand::constant(a.integer));break;
                    default:throw std::invalid_argument("Firestone floating/symbol operand requires custom domain");
                }
            }
            i.targets=source.targets;i.phi_predecessors=source.incoming_blocks;
            if(i.opcode==WO::Nop&&i.result)i.opcode=WO::Input;
            block.instructions.push_back(std::move(i));
        }f.blocks.push_back(std::move(block));
    }f.validate();return f;
}
#endif
}
