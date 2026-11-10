#pragma once
#include "ModRef.hpp"
namespace wetstone::pointer {
inline bool may_write_memory(const ir::Instruction& i){return i.opcode==ir::Opcode::Store||i.opcode==ir::Opcode::Call||i.opcode==ir::Opcode::Free;}
inline bool may_read_memory(const ir::Instruction& i){return i.opcode==ir::Opcode::Load||i.opcode==ir::Opcode::Call;}
inline bool is_barrier(const ir::Instruction& i){return i.opcode==ir::Opcode::Call||i.opcode==ir::Opcode::Free;}
}
