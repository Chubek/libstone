#pragma once
#include "../metacode/archml.hpp"

namespace limestone::bin2bin {
// Decode a byte stream according to the profile's concrete forms and print
// named operands (architectural register numbers and byte-valued displacements).
// This does not infer control flow, perform relocation, or authorize lifting.
Result<std::string> disassemble_archml(const metacode::ArchMLProfile&,std::span<const uint8_t>,uint64_t address=0);
}
