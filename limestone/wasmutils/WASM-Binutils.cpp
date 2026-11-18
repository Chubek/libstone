#include "WASM-Binutils.hpp"

#include <algorithm>
#include <array>
#include <optional>

namespace limestone::wasmutils {
namespace {

// The 36 concrete WASM.aml forms. The *-sleb7 form names in AML have
// assembly mnemonics i32.const and i64.const.
constexpr std::array opcode_table{
    OpcodeInfo{0x00, "unreachable", Operation::unreachable, 0},
    OpcodeInfo{0x01, "nop", Operation::nop, 0},
    OpcodeInfo{0x1a, "drop", Operation::drop, 0},
    OpcodeInfo{0x1b, "select", Operation::select, 0},
    OpcodeInfo{0x41, "i32.const", Operation::constant, 32},
    OpcodeInfo{0x42, "i64.const", Operation::constant, 64},
    OpcodeInfo{0x45, "i32.eqz", Operation::eqz, 32},
    OpcodeInfo{0x50, "i64.eqz", Operation::eqz, 64},
    OpcodeInfo{0x67, "i32.clz", Operation::clz, 32},
    OpcodeInfo{0x68, "i32.ctz", Operation::ctz, 32},
    OpcodeInfo{0x69, "i32.popcnt", Operation::popcnt, 32},
    OpcodeInfo{0x6a, "i32.add", Operation::add, 32},
    OpcodeInfo{0x6b, "i32.sub", Operation::sub, 32},
    OpcodeInfo{0x6c, "i32.mul", Operation::mul, 32},
    OpcodeInfo{0x71, "i32.and", Operation::bit_and, 32},
    OpcodeInfo{0x72, "i32.or", Operation::bit_or, 32},
    OpcodeInfo{0x73, "i32.xor", Operation::bit_xor, 32},
    OpcodeInfo{0x74, "i32.shl", Operation::shl, 32},
    OpcodeInfo{0x75, "i32.shr_s", Operation::shr_s, 32},
    OpcodeInfo{0x76, "i32.shr_u", Operation::shr_u, 32},
    OpcodeInfo{0x77, "i32.rotl", Operation::rotl, 32},
    OpcodeInfo{0x78, "i32.rotr", Operation::rotr, 32},
    OpcodeInfo{0x79, "i64.clz", Operation::clz, 64},
    OpcodeInfo{0x7a, "i64.ctz", Operation::ctz, 64},
    OpcodeInfo{0x7b, "i64.popcnt", Operation::popcnt, 64},
    OpcodeInfo{0x7c, "i64.add", Operation::add, 64},
    OpcodeInfo{0x7d, "i64.sub", Operation::sub, 64},
    OpcodeInfo{0x7e, "i64.mul", Operation::mul, 64},
    OpcodeInfo{0x83, "i64.and", Operation::bit_and, 64},
    OpcodeInfo{0x84, "i64.or", Operation::bit_or, 64},
    OpcodeInfo{0x85, "i64.xor", Operation::bit_xor, 64},
    OpcodeInfo{0x86, "i64.shl", Operation::shl, 64},
    OpcodeInfo{0x87, "i64.shr_s", Operation::shr_s, 64},
    OpcodeInfo{0x88, "i64.shr_u", Operation::shr_u, 64},
    OpcodeInfo{0x89, "i64.rotl", Operation::rotl, 64},
    OpcodeInfo{0x8a, "i64.rotr", Operation::rotr, 64},
};

static_assert(opcode_table.size() == 36);
static_assert([] {
  for (std::size_t i = 0; i < opcode_table.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      if (opcode_table[i].opcode == opcode_table[j].opcode ||
          opcode_table[i].mnemonic == opcode_table[j].mnemonic)
        return false;
    }
  }
  return true;
}());

constexpr auto opcode_index = [] {
  std::array<OpcodeInfo const*, 256> index{};
  for (const auto& info : opcode_table)
    index[info.opcode] = &info;
  return index;
}();

constexpr std::int32_t sleb_min = -64;
constexpr std::int32_t sleb_max = 63;
constexpr std::int32_t sleb_modulus = 128;

const OpcodeInfo& require_opcode(std::uint8_t opcode, std::size_t offset) {
  if (const auto* info = opcode_info(opcode))
    return *info;
  constexpr std::string_view hex = "0123456789abcdef";
  std::string message = "unsupported opcode 0x";
  message += hex[opcode >> 4];
  message += hex[opcode & 0x0f];
  throw Error(offset, message);
}

const OpcodeInfo& check_instruction(const Instruction& instruction) {
  const auto& info = require_opcode(instruction.opcode, instruction.offset);
  if (info.operation == Operation::constant) {
    if (instruction.immediate < sleb_min || instruction.immediate > sleb_max)
      throw Error(instruction.offset, std::string(info.mnemonic) +
          " immediate must be a canonical one-byte signed LEB128 value (-64..63)");
  } else if (instruction.immediate != 0) {
    throw Error(instruction.offset, std::string(info.mnemonic) + " takes no immediate");
  }
  return info;
}

template<class OnInstruction>
void scan(std::span<const std::uint8_t> bytes, std::size_t base,
          OnInstruction on_instruction) {
  std::size_t cursor = 0;
  while (cursor < bytes.size()) {
    const auto offset = base + cursor;
    const auto opcode = bytes[cursor++];
    const auto& info = require_opcode(opcode, offset);
    std::int32_t immediate = 0;
    if (info.operation == Operation::constant) {
      if (cursor == bytes.size())
        throw Error(offset, "truncated " + std::string(info.mnemonic) + " immediate");
      const std::int32_t encoded = bytes[cursor++];
      if (encoded >= sleb_modulus)
        throw Error(offset, std::string(info.mnemonic) +
            " immediate must be canonical one-byte signed LEB128 (continuation bit set)");
      immediate = encoded > sleb_max ? encoded - sleb_modulus : encoded;
    }
    on_instruction(Instruction{opcode, immediate, offset});
  }
}

std::string_view type_name(ValueType type) noexcept {
  return type == ValueType::i32 ? "i32" : "i64";
}

} // namespace

OpcodeInfo const* opcode_info(std::uint8_t opcode) noexcept {
  return opcode_index[opcode];
}

OpcodeInfo const* opcode_info(std::string_view mnemonic) noexcept {
  for (const auto& info : opcode_table)
    if (info.mnemonic == mnemonic)
      return &info;
  return nullptr;
}

Error::Error(std::size_t byte_offset, const std::string& message)
    : std::runtime_error("at byte offset " + std::to_string(byte_offset) + ": " + message),
      offset(byte_offset) {}

namespace {

Validation validate_impl(std::span<const Instruction> instructions,
                         std::optional<std::span<const ValueType>> expected,
                         std::size_t end_offset) {
  Validation result{};
  // Unknown values are possible only in unreachable code. In particular,
  // select can push an explicit unknown when neither candidate has a type.
  std::vector<std::optional<ValueType>> stack;
  for (const auto& instruction : instructions) {
    const auto& info = check_instruction(instruction);
    const auto fail = [&](const std::string& message) {
      throw Error(instruction.offset, std::string(info.mnemonic) + ": " + message);
    };
    const auto pop = [&]() -> std::optional<ValueType> {
      if (stack.empty()) {
        if (!result.always_traps)
          fail("operand stack underflow");
        return std::nullopt; // The bottom of an unreachable stack is polymorphic.
      }
      auto type = stack.back();
      stack.pop_back();
      return type;
    };
    const auto pop_type = [&](ValueType expected) {
      if (auto actual = pop(); actual && *actual != expected)
        fail("type mismatch: expected " + std::string(type_name(expected)) +
             ", found " + std::string(type_name(*actual)));
    };
    const ValueType type = info.width == 64 ? ValueType::i64 : ValueType::i32;
    switch (info.operation) {
    case Operation::unreachable:
      stack.clear();
      result.always_traps = true;
      break;
    case Operation::nop:
      break;
    case Operation::drop:
      pop();
      break;
    case Operation::select: {
      pop_type(ValueType::i32);
      const auto right = pop();
      const auto left = pop();
      if (left && right && *left != *right)
        fail("type mismatch: select candidates must have the same type (found " +
             std::string(type_name(*left)) + " and " + std::string(type_name(*right)) + ")");
      stack.push_back(left ? left : right);
      break;
    }
    case Operation::constant:
      stack.push_back(type);
      break;
    case Operation::eqz:
      pop_type(type);
      stack.push_back(ValueType::i32);
      break;
    case Operation::clz:
    case Operation::ctz:
    case Operation::popcnt:
      pop_type(type);
      stack.push_back(type);
      break;
    case Operation::add:
    case Operation::sub:
    case Operation::mul:
    case Operation::bit_and:
    case Operation::bit_or:
    case Operation::bit_xor:
    case Operation::shl:
    case Operation::shr_s:
    case Operation::shr_u:
    case Operation::rotl:
    case Operation::rotr:
      pop_type(type);
      pop_type(type);
      stack.push_back(type);
      break;
    }
    if (!result.always_traps)
      result.max_stack = std::max(result.max_stack, stack.size());
  }
  if (expected) {
    if (stack.size() > expected->size() ||
        (!result.always_traps && stack.size() != expected->size()))
      throw Error(end_offset, "function body does not match declared result count");
    // Missing (polymorphic) operands lie beneath any known stack entries.
    const auto first = expected->size() - stack.size();
    for (std::size_t i = 0; i < stack.size(); ++i) {
      if (stack[i] && *stack[i] != (*expected)[first + i])
        throw Error(end_offset, "function body does not match declared result type: expected " +
            std::string(type_name((*expected)[first + i])) + ", found " +
            std::string(type_name(*stack[i])));
    }
  }
  if (!result.always_traps) {
    result.results.reserve(stack.size());
    for (const auto type : stack)
      result.results.push_back(*type); // All reachable values have known types.
  }
  return result;
}

} // namespace

Validation validate(std::span<const Instruction> instructions) {
  return validate_impl(instructions, std::nullopt, 0);
}

Validation validate_function(std::span<const Instruction> instructions,
                             std::span<const ValueType> expected_results,
                             std::size_t end_offset) {
  return validate_impl(instructions, expected_results, end_offset);
}

std::vector<std::uint8_t> assemble(std::span<const Instruction> instructions) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(instructions.size());
  for (const auto& instruction : instructions) {
    const auto& info = check_instruction(instruction);
    bytes.push_back(info.opcode);
    if (info.operation == Operation::constant) {
      const auto value = instruction.immediate;
      bytes.push_back(static_cast<std::uint8_t>(value < 0 ? value + sleb_modulus : value));
    }
  }
  return bytes;
}

Program disassemble(std::span<const std::uint8_t> bytes) {
  Program program;
  scan(bytes, 0, [&](Instruction instruction) { program.push_back(instruction); });
  return program;
}

std::vector<std::uint8_t> link(
    std::span<const std::span<const std::uint8_t>> fragments) {
  std::vector<std::uint8_t> result;
  std::size_t total = 0;
  for (const auto fragment : fragments) {
    if (fragment.size() > result.max_size() - total)
      throw Error(total, "linked stream exceeds maximum size");
    scan(fragment, total, [](Instruction) {});
    total += fragment.size();
  }
  result.reserve(total);
  for (const auto fragment : fragments)
    if (!fragment.empty())
      result.insert(result.end(), fragment.begin(), fragment.end());
  return result;
}

} // namespace limestone::wasmutils
