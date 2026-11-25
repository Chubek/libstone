#include "WASM-SIMD.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace limestone::wasmutils::core::detail {
namespace {

using Word = std::uint64_t;

// Lane 0 occupies the low bits on every host, independently of host byte order.
struct Packed {
  Word low{}, high{};
};

constexpr Word mask(unsigned width) {
  return std::numeric_limits<Word>::max() >> (64 - width);
}

constexpr Word sign_bit(unsigned width) {
  return Word{1} << (width - 1);
}

constexpr Word extend_signed(Word value, unsigned width) {
  return (value ^ sign_bit(width)) - sign_bit(width);
}

constexpr std::int64_t signed_lane(Word value, unsigned width) {
  return std::bit_cast<std::int64_t>(extend_signed(value, width));
}

Word get_lane(Packed value, unsigned width, unsigned lane) {
  const auto bit = lane * width;
  return ((bit < 64 ? value.low : value.high) >> (bit % 64)) & mask(width);
}

void set_lane(Packed& value, unsigned width, unsigned lane, Word bits) {
  const auto bit = lane * width;
  const auto shift = bit % 64;
  auto& half = bit < 64 ? value.low : value.high;
  half = (half & ~(mask(width) << shift)) | ((bits & mask(width)) << shift);
}

template<class Read>
Packed pack(unsigned width, Read read, unsigned count) {
  Packed value;
  for (unsigned i = 0; i < count; ++i)
    set_lane(value, width, i, read(i));
  return value;
}

template<class Read>
Packed pack(unsigned width, Read read) {
  return pack(width, read, 128 / width);
}

Value pop(std::vector<Value>& stack) {
  const auto value = stack.back();
  stack.pop_back();
  return value;
}

Packed pop_vector(std::vector<Value>& stack) {
  const auto value = pop(stack);
  return {value.bits, value.high};
}

void push(std::vector<Value>& stack, Packed value) {
  stack.push_back({Type::v128, value.low, value.high});
}

void push_i32(std::vector<Value>& stack, std::uint32_t bits) {
  stack.push_back({Type::i32, bits});
}

template<class Operation>
bool unary(std::vector<Value>& stack, unsigned width, Operation operation) {
  const auto value = pop_vector(stack);
  push(stack, pack(width, [&](unsigned i) { return operation(get_lane(value, width, i)); }));
  return true;
}

template<class Operation>
bool binary(std::vector<Value>& stack, unsigned width, Operation operation) {
  const auto rhs = pop_vector(stack);
  const auto lhs = pop_vector(stack);
  push(stack, pack(width, [&](unsigned i) {
    return operation(get_lane(lhs, width, i), get_lane(rhs, width, i));
  }));
  return true;
}

Word read_bytes(std::span<const std::uint8_t> bytes) {
  Word result = 0;
  for (unsigned i = 0; i < bytes.size(); ++i)
    result |= Word{bytes[i]} << (8 * i);
  return result;
}

void write_bytes(std::span<std::uint8_t> bytes, Word value) {
  for (auto& byte : bytes) {
    byte = static_cast<std::uint8_t>(value);
    value >>= 8;
  }
}

Packed read_vector(std::span<const std::uint8_t> bytes) {
  return {read_bytes(bytes.first(8)), read_bytes(bytes.subspan(8, 8))};
}

std::span<std::uint8_t> memory_range(std::span<std::uint8_t> memory, Word address,
                                     MemoryArg arg, unsigned size, std::size_t offset) {
  // The Wasm32 address plus the static offset does not wrap to 32 bits.
  const Word pos = Word{static_cast<std::uint32_t>(address)} + arg.offset;
  if (pos > memory.size() || size > memory.size() - pos)
    throw Trap(offset, "SIMD memory access out of bounds");
  return memory.subspan(static_cast<std::size_t>(pos), size);
}

void memory_operation(const Instruction& instruction, std::vector<Value>& stack,
                      std::span<std::uint8_t> memory) {
  const auto op = instruction.opcode.extension;
  if (op >= 0x54 && op <= 0x5b) {
    const auto immediate = std::get<MemoryLane>(instruction.immediate);
    const auto size = 1u << ((op - 0x54) % 4);
    auto value = pop_vector(stack);
    const auto bytes = memory_range(memory, pop(stack).bits, immediate.memory, size,
                                    instruction.offset);
    if (op < 0x58) {
      set_lane(value, size * 8, immediate.lane, read_bytes(bytes));
      push(stack, value);
    } else {
      write_bytes(bytes, get_lane(value, size * 8, immediate.lane));
    }
    return;
  }

  const auto immediate = std::get<MemoryArg>(instruction.immediate);
  const auto access = [&](unsigned size) {
    return memory_range(memory, pop(stack).bits, immediate, size, instruction.offset);
  };
  if (op == 0x0b) {
    const auto value = pop_vector(stack);
    const auto bytes = access(16);
    write_bytes(bytes.first(8), value.low);
    write_bytes(bytes.last(8), value.high);
  } else if (op == 0x00) {
    push(stack, read_vector(access(16)));
  } else if (op <= 0x06) {
    const auto source = access(8);
    const auto size = 1u << ((op - 1) / 2);
    push(stack, pack(size * 16, [&](unsigned i) {
      const auto bits = read_bytes(source.subspan(i * size, size));
      return (op & 1) != 0 ? extend_signed(bits, size * 8) : bits;
    }));
  } else if (op <= 0x0a) {
    const auto size = 1u << (op - 7);
    const auto bits = read_bytes(access(size));
    push(stack, pack(size * 8, [=](unsigned) { return bits; }));
  } else {
    push(stack, {read_bytes(access(op == 0x5c ? 4 : 8)), 0});
  }
}

struct LaneForm {
  unsigned width;
  Type scalar;
  bool replace = false;
  bool extend = false;
};

void lane_operation(const Instruction& instruction, std::vector<Value>& stack) {
  constexpr LaneForm forms[] = {
    {8, Type::i32, false, true}, {8, Type::i32}, {8, Type::i32, true},
    {16, Type::i32, false, true}, {16, Type::i32}, {16, Type::i32, true},
    {32, Type::i32}, {32, Type::i32, true}, {64, Type::i64}, {64, Type::i64, true},
    {32, Type::f32}, {32, Type::f32, true}, {64, Type::f64}, {64, Type::f64, true},
  };
  const auto [width, type, replace, extend] = forms[instruction.opcode.extension - 0x15];
  const auto index = std::get<std::uint8_t>(instruction.immediate);
  if (replace) {
    const auto bits = pop(stack).bits;
    auto value = pop_vector(stack);
    set_lane(value, width, index, bits);
    push(stack, value);
  } else {
    auto bits = get_lane(pop_vector(stack), width, index);
    if (extend)
      bits = static_cast<std::uint32_t>(extend_signed(bits, width));
    stack.push_back({type, bits});
  }
}

void shuffle(const Instruction& instruction, std::vector<Value>& stack) {
  const auto rhs = pop_vector(stack);
  const auto lhs = pop_vector(stack);
  if (instruction.opcode.extension == 0x0d) {
    const auto& indices = std::get<VectorBytes>(instruction.immediate);
    push(stack, pack(8, [&](unsigned i) {
      const auto index = indices[i];
      return index < 16 ? get_lane(lhs, 8, index) : get_lane(rhs, 8, index - 16);
    }));
  } else {
    push(stack, pack(8, [&](unsigned i) {
      const auto index = static_cast<unsigned>(get_lane(rhs, 8, i));
      return index < 16 ? get_lane(lhs, 8, index) : Word{0};
    }));
  }
}

bool compare_integer(std::vector<Value>& stack, unsigned width, unsigned kind) {
  return binary(stack, width, [=](Word a, Word b) {
    if ((kind & 1) == 0 && kind >= 2) {
      // Flipping the sign bit maps signed order to unsigned order.
      a ^= sign_bit(width);
      b ^= sign_bit(width);
    }
    bool match;
    switch (kind) {
    case 0: match = a == b; break;
    case 1: match = a != b; break;
    case 2: case 3: match = a < b; break;
    case 4: case 5: match = a > b; break;
    case 6: case 7: match = a <= b; break;
    default: match = a >= b; break;
    }
    return match ? mask(width) : Word{0};
  });
}

bool compare_float(std::vector<Value>& stack, unsigned width, std::uint8_t op) {
  return binary(stack, width, [=](Word a, Word b) {
    return evaluate_scalar({op}, a, b) != 0 ? mask(width) : Word{0};
  });
}

bool float_unary(std::vector<Value>& stack, unsigned width, std::uint8_t op) {
  return unary(stack, width, [=](Word a) { return evaluate_scalar({op}, a, 0); });
}

bool float_binary(std::vector<Value>& stack, unsigned width, std::uint8_t op) {
  return binary(stack, width, [=](Word a, Word b) { return evaluate_scalar({op}, a, b); });
}

bool pseudo_minmax(std::vector<Value>& stack, unsigned width, bool minimum) {
  const Opcode lt{width == 32 ? std::uint8_t{0x5d} : std::uint8_t{0x63}};
  return binary(stack, width, [=](Word a, Word b) {
    // Ties and unordered comparisons select the first argument, preserving
    // the exact NaN payload and signed-zero bits if that argument is selected.
    return (minimum ? evaluate_scalar(lt, b, a) : evaluate_scalar(lt, a, b)) != 0 ? b : a;
  });
}

bool convert(std::vector<Value>& stack, unsigned source, unsigned destination, Opcode op) {
  const auto input = pop_vector(stack);
  push(stack, pack(destination, [&](unsigned i) {
    return evaluate_scalar(op, get_lane(input, source, i), 0);
  }, 128 / std::max(source, destination)));
  return true;
}

Word saturate(std::int64_t value, unsigned width, bool is_signed) {
  const auto lower = is_signed ? -static_cast<std::int64_t>(sign_bit(width)) : 0;
  const auto upper = static_cast<std::int64_t>(mask(width - (is_signed ? 1 : 0)));
  return static_cast<Word>(std::clamp(value, lower, upper));
}

bool narrowing(std::vector<Value>& stack, unsigned width, bool is_signed) {
  const auto rhs = pop_vector(stack);
  const auto lhs = pop_vector(stack);
  const auto half = 64 / width;
  push(stack, pack(width, [&](unsigned i) {
    const auto bits = get_lane(i < half ? lhs : rhs, width * 2, i % half);
    // The source lanes are signed even for narrow_*_u, whose result is unsigned.
    return saturate(signed_lane(bits, width * 2), width, is_signed);
  }));
  return true;
}

Word widened_lane(Packed input, unsigned width, unsigned i, bool is_signed) {
  const auto bits = get_lane(input, width, i);
  return is_signed ? extend_signed(bits, width) : bits;
}

bool widening(std::vector<Value>& stack, unsigned width, unsigned operation, bool multiply) {
  const auto rhs = pop_vector(stack);
  const auto lhs = multiply ? pop_vector(stack) : Packed{};
  const auto half_start = (operation & 1) != 0 ? 128 / width : 0;
  const auto is_signed = operation < 2;
  push(stack, pack(width, [&](unsigned i) {
    const auto index = i + half_start;
    const auto right = widened_lane(rhs, width / 2, index, is_signed);
    return multiply ? widened_lane(lhs, width / 2, index, is_signed) * right : right;
  }));
  return true;
}

bool pairwise_add(std::vector<Value>& stack, unsigned op) {
  const auto width = op < 0x7e ? 16u : 32u;
  const auto is_signed = (op & 1) == 0;
  const auto input = pop_vector(stack);
  push(stack, pack(width, [&](unsigned i) {
    return widened_lane(input, width / 2, i * 2, is_signed) +
           widened_lane(input, width / 2, i * 2 + 1, is_signed);
  }));
  return true;
}

bool dot_product(std::vector<Value>& stack) {
  const auto rhs = pop_vector(stack);
  const auto lhs = pop_vector(stack);
  const auto product = [&](unsigned lane) {
    return widened_lane(lhs, 16, lane, true) * widened_lane(rhs, 16, lane, true);
  };
  push(stack, pack(32, [&](unsigned i) { return product(i * 2) + product(i * 2 + 1); }));
  return true;
}

bool q15_multiply(std::vector<Value>& stack) {
  return binary(stack, 16, [](Word a, Word b) {
    const auto product = signed_lane(a, 16) * signed_lane(b, 16);
    return saturate((product + 0x4000) >> 15, 16, true);
  });
}

Word integer_arithmetic(unsigned slot, unsigned width, Word a, Word b) {
  switch (slot) {
  case 0x0e: return a + b;
  case 0x0f: return saturate(signed_lane(a, width) + signed_lane(b, width), width, true);
  case 0x10: return saturate(static_cast<std::int64_t>(a + b), width, false);
  case 0x11: return a - b;
  case 0x12: return saturate(signed_lane(a, width) - signed_lane(b, width), width, true);
  case 0x13: return a >= b ? a - b : 0;
  case 0x15: return a * b;
  case 0x16: return signed_lane(a, width) < signed_lane(b, width) ? a : b;
  case 0x17: return std::min(a, b);
  case 0x18: return signed_lane(a, width) > signed_lane(b, width) ? a : b;
  case 0x19: return std::max(a, b);
  default: return (a + b + 1) >> 1; // 0x1b: rounded unsigned average
  }
}

bool integer_simd(std::vector<Value>& stack, unsigned op) {
  // The integer families repeat every 0x20 opcodes. Special instructions
  // occupying holes in this layout are handled before reaching this function.
  const auto width = 8u << ((op - 0x60) / 0x20);
  const auto slot = op % 0x20;
  switch (slot) {
  case 0x00:
    return unary(stack, width, [=](Word a) { return (a & sign_bit(width)) ? 0 - a : a; });
  case 0x01:
    return unary(stack, width, [](Word a) { return 0 - a; });
  case 0x02: // popcnt (i8x16)
    return unary(stack, 8, [](Word a) { return std::popcount(a); });
  case 0x03: case 0x04: { // all_true, bitmask
    const auto value = pop_vector(stack);
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 128 / width; ++i) {
      const auto bits = get_lane(value, width, i);
      const auto flag = slot == 0x03 ? bits != 0 : (bits & sign_bit(width)) != 0;
      result |= std::uint32_t{flag} << i;
    }
    push_i32(stack, slot == 0x03 ? result == mask(128 / width) : result);
    return true;
  }
  case 0x05: case 0x06:
    return narrowing(stack, width, slot == 0x05);
  case 0x07: case 0x08: case 0x09: case 0x0a:
    return widening(stack, width, slot - 0x07, false);
  case 0x0b: case 0x0c: case 0x0d: {
    const auto shift = pop(stack).bits & (width - 1);
    return unary(stack, width, [=](Word a) {
      if (slot == 0x0b)
        return a << shift;
      const auto fill = slot == 0x0c && (a & sign_bit(width)) != 0 ? mask(width) : 0;
      return ((a ^ fill) >> shift) ^ fill;
    });
  }
  case 0x0e: case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13:
  case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1b:
    return binary(stack, width, [=](Word a, Word b) { return integer_arithmetic(slot, width, a, b); });
  case 0x1c: case 0x1d: case 0x1e: case 0x1f:
    return widening(stack, width, slot - 0x1c, true);
  default:
    return false;
  }
}

} // namespace

bool execute_simd(const Instruction& instruction, std::vector<Value>& stack,
                  std::span<std::uint8_t> memory) {
  const auto opcode = instruction.opcode;
  if (opcode.byte != 0xfd || !lookup(opcode))
    return false;
  const auto op = opcode.extension;
  if (op <= 0x0b || (op >= 0x54 && op <= 0x5d)) {
    memory_operation(instruction, stack, memory);
    return true;
  }
  if (op == 0x0c) {
    push(stack, read_vector(std::get<VectorBytes>(instruction.immediate)));
    return true;
  }
  if (op == 0x0d || op == 0x0e) {
    shuffle(instruction, stack);
    return true;
  }
  if (op >= 0x0f && op <= 0x14) {
    constexpr std::array widths{8u, 16u, 32u, 64u, 32u, 64u};
    const auto bits = pop(stack).bits;
    push(stack, pack(widths[op - 0x0f], [=](unsigned) { return bits; }));
    return true;
  }
  if (op >= 0x15 && op <= 0x22) {
    lane_operation(instruction, stack);
    return true;
  }
  if (op >= 0x23 && op <= 0x40)
    return compare_integer(stack, 8u << ((op - 0x23) / 10), (op - 0x23) % 10);
  if (op >= 0x41 && op <= 0x46)
    return compare_float(stack, 32, static_cast<std::uint8_t>(0x5b + op - 0x41));
  if (op >= 0x47 && op <= 0x4c)
    return compare_float(stack, 64, static_cast<std::uint8_t>(0x61 + op - 0x47));
  if (op >= 0x67 && op <= 0x6a)
    return float_unary(stack, 32, static_cast<std::uint8_t>(0x8d + op - 0x67));
  if (op >= 0xd6 && op <= 0xdb) {
    constexpr std::array kinds{0u, 1u, 2u, 4u, 6u, 8u};
    return compare_integer(stack, 64, kinds[op - 0xd6]);
  }
  if (op >= 0xe4 && op <= 0xe9)
    return float_binary(stack, 32, static_cast<std::uint8_t>(0x92 + op - 0xe4));
  if (op >= 0xf0 && op <= 0xf5)
    return float_binary(stack, 64, static_cast<std::uint8_t>(0xa0 + op - 0xf0));
  switch (op) {
  case 0x4d: return unary(stack, 64, [](Word a) { return ~a; });
  case 0x4e: return binary(stack, 64, [](Word a, Word b) { return a & b; });
  case 0x4f: return binary(stack, 64, [](Word a, Word b) { return a & ~b; });
  case 0x50: return binary(stack, 64, [](Word a, Word b) { return a | b; });
  case 0x51: return binary(stack, 64, [](Word a, Word b) { return a ^ b; });
  case 0x52: {
    const auto selector = pop_vector(stack);
    const auto rhs = pop_vector(stack);
    const auto lhs = pop_vector(stack);
    push(stack, {(lhs.low & selector.low) | (rhs.low & ~selector.low),
                 (lhs.high & selector.high) | (rhs.high & ~selector.high)});
    return true;
  }
  case 0x53: {
    const auto value = pop_vector(stack);
    push_i32(stack, (value.low | value.high) != 0);
    return true;
  }
  case 0x5e: return convert(stack, 64, 32, {0xb6}); // demote
  case 0x5f: return convert(stack, 32, 64, {0xbb}); // promote low
  case 0x74: return float_unary(stack, 64, 0x9b); // ceil
  case 0x75: return float_unary(stack, 64, 0x9c); // floor
  case 0x7a: return float_unary(stack, 64, 0x9d); // trunc
  case 0x7c: case 0x7d: case 0x7e: case 0x7f:
    return pairwise_add(stack, op);
  case 0x82: return q15_multiply(stack);
  case 0x94: return float_unary(stack, 64, 0x9e); // nearest
  case 0xba: return dot_product(stack);
  case 0xe0: return float_unary(stack, 32, 0x8b); // abs
  case 0xe1: return float_unary(stack, 32, 0x8c); // neg
  case 0xe3: return float_unary(stack, 32, 0x91); // sqrt
  case 0xea: return pseudo_minmax(stack, 32, true);
  case 0xeb: return pseudo_minmax(stack, 32, false);
  case 0xec: return float_unary(stack, 64, 0x99); // abs
  case 0xed: return float_unary(stack, 64, 0x9a); // neg
  case 0xef: return float_unary(stack, 64, 0x9f); // sqrt
  case 0xf6: return pseudo_minmax(stack, 64, true);
  case 0xf7: return pseudo_minmax(stack, 64, false);
  case 0xf8: return convert(stack, 32, 32, {0xfc, 0}); // trunc_sat_f32_s
  case 0xf9: return convert(stack, 32, 32, {0xfc, 1}); // trunc_sat_f32_u
  case 0xfa: return convert(stack, 32, 32, {0xb2}); // convert_i32_s
  case 0xfb: return convert(stack, 32, 32, {0xb3}); // convert_i32_u
  case 0xfc: return convert(stack, 64, 32, {0xfc, 2}); // trunc_sat_f64_s_zero
  case 0xfd: return convert(stack, 64, 32, {0xfc, 3}); // trunc_sat_f64_u_zero
  case 0xfe: return convert(stack, 32, 64, {0xb7}); // convert_low_i32_s
  case 0xff: return convert(stack, 32, 64, {0xb8}); // convert_low_i32_u
  default:
    return op >= 0x60 && op <= 0xdf && integer_simd(stack, op);
  }
}

} // namespace limestone::wasmutils::core::detail
