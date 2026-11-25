#pragma once

#include "WASM-Instance.hpp"

namespace limestone::wasmutils::core::detail {

// Execute one validated SIMD instruction, returning false for a non-SIMD opcode.
bool execute_simd(const Instruction& instruction, std::vector<Value>& stack,
                  std::span<std::uint8_t> memory);

// Reuse the interpreter's scalar floating and numeric semantics for SIMD lanes.
std::uint64_t evaluate_scalar(Opcode opcode, std::uint64_t lhs, std::uint64_t rhs);

} // namespace limestone::wasmutils::core::detail
