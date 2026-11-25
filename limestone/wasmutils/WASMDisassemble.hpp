#pragma once

#include "WASM-IR.hpp"
#include "WAT-Print.hpp"

namespace limestone::wasmutils {

// Decode a raw instruction stream, recording each opcode's byte offset.
// Reject unsupported/truncated encodings with Error at the instruction start;
// does not check stack effects.
Program disassemble(std::span<const std::uint8_t> bytes);

} // namespace limestone::wasmutils
