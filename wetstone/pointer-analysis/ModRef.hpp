#pragma once
#include "../utils/IR.hpp"
namespace wetstone::pointer {
struct ModRef {bool reads=false,writes=false,calls=false,allocates=false,frees=false;};
inline ModRef effects(const ir::Function& f){ModRef r;
    for(const auto& b:f.blocks)for(const auto& i:b.instructions)switch(i.opcode){
        case ir::Opcode::Load:r.reads=true;break;case ir::Opcode::Store:r.writes=true;break;
        case ir::Opcode::Call:r.calls=true;r.reads=true;r.writes=true;break;
        case ir::Opcode::Alloc:r.allocates=true;break;case ir::Opcode::Free:r.frees=true;break;
        default:break;
    }return r;
}
}
