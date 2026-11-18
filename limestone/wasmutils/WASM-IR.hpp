#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace limestone::wasmutils {

enum class ValueType { i32, i64 };

enum class Operation {
  unreachable, nop, drop, select, constant, eqz, clz, ctz, popcnt,
  add, sub, mul, bit_and, bit_or, bit_xor, shl, shr_s, shr_u, rotl, rotr
};

struct Instruction {
  std::uint8_t opcode{};
  std::int32_t immediate{}; // Zero for instructions that do not take an immediate.
  std::size_t offset{};     // Source byte offset, not used to place instructions.
};

using Program = std::vector<Instruction>;

struct OpcodeInfo {
  std::uint8_t opcode;
  std::string_view mnemonic;
  Operation operation;
  unsigned width; // Integer operand width in bits, or zero for untyped forms.
};

// Return a stable descriptor for one of the selected WASM.aml forms, or nullptr.
OpcodeInfo const* opcode_info(std::uint8_t opcode) noexcept;
OpcodeInfo const* opcode_info(std::string_view mnemonic) noexcept;

class Error : public std::runtime_error {
public:
  std::size_t offset;
  Error(std::size_t offset, const std::string& message);
};

struct Validation {
  std::vector<ValueType> results; // Fallthrough stack, bottom to top; empty on trap.
  std::size_t max_stack;          // Peak reachable stack depth in values.
  bool always_traps;
};

// Check the selected forms and their stack effects from an empty entry stack.
// Even after unreachable, known operand types must match. No module or function
// context is assumed; any fallthrough stack is returned as the result.
Validation validate(std::span<const Instruction> instructions);

// Check the declared results at a zero-argument function's implicit end.
// A polymorphic tail may supply missing results on an unreachable path, but
// explicit stack values must match and no extra operands may remain.
Validation validate_function(std::span<const Instruction> instructions,
                             std::span<const ValueType> expected_results,
                             std::size_t end_offset);

} // namespace limestone::wasmutils
