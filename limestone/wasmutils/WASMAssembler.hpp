#pragma once

#include "WASM-IR.hpp"
#include "WAT-Module.hpp"

namespace limestone::wasmutils {

// Encode a raw instruction stream (no module header or implicit terminator).
// Reject unsupported encodings with Error; does not check stack effects.
std::vector<std::uint8_t> assemble(std::span<const Instruction> instructions);

} // namespace limestone::wasmutils
