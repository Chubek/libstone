#include "WASM-Operations.hpp"
#include "WASM-SIMD.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

namespace limestone::wasmutils::core {
namespace {

// Use the unsigned representation everywhere that WebAssembly specifies
// modular arithmetic. Only convert to signed after checking exceptional cases.
template<class UInt>
using Signed = std::make_signed_t<UInt>;

template<class UInt>
constexpr auto word_width = std::numeric_limits<UInt>::digits;

template<class UInt>
constexpr UInt sign_bit = UInt{1} << (word_width<UInt> - 1);

template<class UInt>
UInt sign_extend(UInt bits, unsigned from_width) {
  const auto sign = UInt{1} << (from_width - 1);
  const auto mask = sign | (sign - 1);
  return ((bits & mask) ^ sign) - sign;
}

template<class UInt>
bool integer_compare(unsigned operation, UInt lhs, UInt rhs) {
  // Flipping the sign bit maps the signed order to the unsigned order.
  switch (operation) {
    case 0: return lhs == rhs;
    case 1: return lhs != rhs;
    case 2: return (lhs ^ sign_bit<UInt>) < (rhs ^ sign_bit<UInt>);
    case 3: return lhs < rhs;
    case 4: return (lhs ^ sign_bit<UInt>) > (rhs ^ sign_bit<UInt>);
    case 5: return lhs > rhs;
    case 6: return (lhs ^ sign_bit<UInt>) <= (rhs ^ sign_bit<UInt>);
    case 7: return lhs <= rhs;
    case 8: return (lhs ^ sign_bit<UInt>) >= (rhs ^ sign_bit<UInt>);
    case 9: return lhs >= rhs;
    default: throw std::logic_error("not a Wasm integer comparison");
  }
}

template<class UInt>
UInt integer_count(unsigned operation, UInt operand) {
  switch (operation) {
    case 0: return static_cast<UInt>(std::countl_zero(operand));
    case 1: return static_cast<UInt>(std::countr_zero(operand));
    case 2: return static_cast<UInt>(std::popcount(operand));
    default: throw std::logic_error("not a Wasm bit count");
  }
}

enum class NumericFault { none, divide_by_zero, overflow, invalid_conversion };

struct NumericResult {
  std::uint64_t bits{};
  NumericFault fault{};
};

template<class UInt>
NumericResult integer_binary(unsigned operation, UInt lhs, UInt rhs) {
  const auto shift = static_cast<int>(rhs & (word_width<UInt> - 1));
  const auto a = std::uint64_t{lhs};
  const auto b = std::uint64_t{rhs};
  switch (operation) {
    case 0: return {static_cast<UInt>(a + b)};
    case 1: return {static_cast<UInt>(a - b)};
    case 2: return {static_cast<UInt>(a * b)};
    case 3: // div_s
    case 5: // rem_s
      if (rhs == 0)
        return {0, NumericFault::divide_by_zero};
      if (lhs == sign_bit<UInt> && rhs == std::numeric_limits<UInt>::max())
        // Only the signed division overflows; the remainder is exactly zero.
        return {0, operation == 3 ? NumericFault::overflow : NumericFault::none};
      if (operation == 3)
        return {static_cast<UInt>(std::bit_cast<Signed<UInt>>(lhs) /
                                 std::bit_cast<Signed<UInt>>(rhs))};
      return {static_cast<UInt>(std::bit_cast<Signed<UInt>>(lhs) %
                               std::bit_cast<Signed<UInt>>(rhs))};
    case 4: // div_u
    case 6: // rem_u
      if (rhs == 0)
        return {0, NumericFault::divide_by_zero};
      return {operation == 4 ? lhs / rhs : lhs % rhs};
    case 7: return {lhs & rhs};
    case 8: return {lhs | rhs};
    case 9: return {lhs ^ rhs};
    case 10: return {static_cast<UInt>(a << shift)};
    case 11:
      // Complement the negative case before and after shifting to fill with ones.
      return {(lhs & sign_bit<UInt>) != 0
          ? static_cast<UInt>(~(static_cast<UInt>(~lhs) >> shift))
          : static_cast<UInt>(lhs >> shift)};
    case 12: return {lhs >> shift};
    case 13: return {std::rotl(lhs, shift)};
    case 14: return {std::rotr(lhs, shift)};
    default: throw std::logic_error("not a Wasm integer binary operation");
  }
}

static_assert(sizeof(float) == sizeof(std::uint32_t) &&
              std::numeric_limits<float>::is_iec559 && std::numeric_limits<float>::digits == 24);
static_assert(sizeof(double) == sizeof(std::uint64_t) &&
              std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);

template<class Float>
using FloatBits = std::conditional_t<std::is_same_v<Float, float>, std::uint32_t, std::uint64_t>;

template<class Float>
constexpr auto canonical_nan = std::is_same_v<Float, float>
    ? FloatBits<Float>{0x7fc00000}
    : static_cast<FloatBits<Float>>(0x7ff8000000000000ULL);

// Wasm uses round-to-nearest-even and admits neither host FP traps nor flushing
// of subnormals. Restore the caller's flags and modes before continuing (host
// functions run outside these scopes). Volatile operands/results keep the
// actual arithmetic and any narrowing inside the guarded scope.
class WasmFloatEnvironment {
 public:
  WasmFloatEnvironment() : errno_(errno) {
    if (std::fegetenv(&previous_) != 0 || std::fesetenv(FE_DFL_ENV) != 0)
      throw std::runtime_error("could not set the Wasm floating-point environment");
  }
  ~WasmFloatEnvironment() {
    std::fesetenv(&previous_);
    errno = errno_;
  }
  WasmFloatEnvironment(const WasmFloatEnvironment&) = delete;
  WasmFloatEnvironment& operator=(const WasmFloatEnvironment&) = delete;

 private:
  std::fenv_t previous_;
  int errno_;
};

template<class Float>
FloatBits<Float> float_result(Float result) {
  // The specification permits this canonical choice for arithmetic NaNs.
  return std::isnan(result) ? canonical_nan<Float> : std::bit_cast<FloatBits<Float>>(result);
}

template<class Float>
bool float_compare(unsigned operation, FloatBits<Float> lhs, FloatBits<Float> rhs) {
  const WasmFloatEnvironment environment;
  const volatile Float a = std::bit_cast<Float>(lhs);
  const volatile Float b = std::bit_cast<Float>(rhs);
  switch (operation) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a < b;
    case 3: return a > b;
    case 4: return a <= b;
    case 5: return a >= b;
    default: throw std::logic_error("not a Wasm float comparison");
  }
}

template<class Float>
FloatBits<Float> float_unary(unsigned operation, FloatBits<Float> bits) {
  using UInt = FloatBits<Float>;
  if (operation == 0) // abs
    return bits & ~sign_bit<UInt>;
  if (operation == 1) // neg
    return bits ^ sign_bit<UInt>;

  const WasmFloatEnvironment environment;
  const volatile Float operand = std::bit_cast<Float>(bits);
  const volatile Float result = [&]() -> Float {
    switch (operation) {
      case 2: return std::ceil(operand);
      case 3: return std::floor(operand);
      case 4: return std::trunc(operand);
      case 5: return std::nearbyint(operand);
      case 6: return std::sqrt(operand);
      default: throw std::logic_error("not a Wasm float unary operation");
    }
  }();
  return float_result<Float>(result);
}

template<class Float>
FloatBits<Float> float_binary(unsigned operation, FloatBits<Float> lhs, FloatBits<Float> rhs) {
  using UInt = FloatBits<Float>;
  if (operation == 6) // copysign preserves even a signaling NaN's payload.
    return (lhs & ~sign_bit<UInt>) | (rhs & sign_bit<UInt>);

  const WasmFloatEnvironment environment;
  const volatile Float a = std::bit_cast<Float>(lhs);
  const volatile Float b = std::bit_cast<Float>(rhs);
  if (operation == 4 || operation == 5) { // min, max
    if (std::isnan(a) || std::isnan(b))
      return canonical_nan<Float>;
    if (a == b)
      // Equal nonzeros have identical bits. For zeros, min prefers -0 and max +0.
      return operation == 4 ? lhs | rhs : lhs & rhs;
    return (operation == 4 ? a < b : a > b) ? lhs : rhs;
  }
  const volatile Float result = [&]() -> Float {
    switch (operation) {
      case 0: return a + b;
      case 1: return a - b;
      case 2: return a * b;
      case 3: return a / b;
      default: throw std::logic_error("not a Wasm float binary operation");
    }
  }();
  return float_result<Float>(result);
}

template<class UInt, class Float>
NumericResult truncate_float(FloatBits<Float> bits, bool is_signed, bool saturate) {
  const WasmFloatEnvironment environment;
  const volatile Float operand = std::bit_cast<Float>(bits);
  if (std::isnan(operand))
    return {0, saturate ? NumericFault::none : NumericFault::invalid_conversion};
  const auto truncated = std::trunc(operand);
  // The exclusive upper bound is an exact power of two; unlike converting
  // MAX to a float, the test cannot accidentally accept that rounded bound.
  constexpr auto signed_limit = static_cast<Float>(sign_bit<UInt>);
  const auto lower = is_signed ? -signed_limit : Float{0};
  const auto upper = is_signed ? signed_limit : signed_limit * Float{2};
  if (truncated < lower)
    return {is_signed && saturate ? sign_bit<UInt> : UInt{0},
            saturate ? NumericFault::none : NumericFault::overflow};
  if (truncated >= upper) {
    const auto maximum = is_signed ? sign_bit<UInt> - 1 : std::numeric_limits<UInt>::max();
    return {saturate ? maximum : UInt{0}, saturate ? NumericFault::none : NumericFault::overflow};
  }
  return {is_signed ? static_cast<UInt>(static_cast<Signed<UInt>>(truncated))
                    : static_cast<UInt>(truncated)};
}

template<class Float, class From>
FloatBits<Float> convert_float(From operand) {
  const WasmFloatEnvironment environment;
  const volatile From source = operand;
  const volatile Float result = static_cast<Float>(source);
  return float_result<Float>(result);
}

std::uint32_t demote_float(std::uint64_t bits) {
  // Deal explicitly with finite values above the range of float. The
  // round-to-nearest overflow boundary is halfway from MAX to 2^128.
  constexpr auto f32_max = std::bit_cast<std::uint64_t>(0x1.fffffep127);
  constexpr auto f32_overflow = std::bit_cast<std::uint64_t>(0x1.ffffffp127);
  constexpr auto f64_infinity = std::bit_cast<std::uint64_t>(
      std::numeric_limits<double>::infinity());
  const auto magnitude = bits & ~sign_bit<std::uint64_t>;
  if (magnitude > f64_infinity)
    return canonical_nan<float>;
  if (magnitude > f32_max) {
    const auto sign = static_cast<std::uint32_t>(bits >> 32) & sign_bit<std::uint32_t>;
    return sign | (magnitude >= f32_overflow ? 0x7f800000 : 0x7f7fffff);
  }
  return convert_float<float>(std::bit_cast<double>(bits));
}

// The eight trapping and eight saturating conversions use the same ordering:
// output width, then input width, then signed/unsigned.
NumericResult float_to_integer(unsigned operation, std::uint64_t bits, bool saturate) {
  const bool is_signed = (operation & 1) == 0;
  const bool from_f32 = (operation & 2) == 0;
  const auto low = static_cast<std::uint32_t>(bits);
  if (operation < 4)
    return from_f32 ? truncate_float<std::uint32_t, float>(low, is_signed, saturate)
                    : truncate_float<std::uint32_t, double>(bits, is_signed, saturate);
  return from_f32 ? truncate_float<std::uint64_t, float>(low, is_signed, saturate)
                  : truncate_float<std::uint64_t, double>(bits, is_signed, saturate);
}

template<class Float>
FloatBits<Float> integer_to_float(unsigned operation, std::uint64_t bits) {
  switch (operation) {
    case 0: return convert_float<Float>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits)));
    case 1: return convert_float<Float>(static_cast<std::uint32_t>(bits));
    case 2: return convert_float<Float>(std::bit_cast<std::int64_t>(bits));
    case 3: return convert_float<Float>(bits);
    default: throw std::logic_error("not a Wasm integer-to-float conversion");
  }
}

NumericResult evaluate_numeric(const Instruction& instruction, std::uint64_t lhs, std::uint64_t rhs) {
  const auto byte = instruction.opcode.byte;
  const auto a32 = static_cast<std::uint32_t>(lhs);
  const auto b32 = static_cast<std::uint32_t>(rhs);
  if (byte == 0xfc)
    return float_to_integer(instruction.opcode.extension, lhs, true);
  switch (byte) {
    case 0x41: case 0x43: return {std::get<std::uint32_t>(instruction.immediate)};
    case 0x42: case 0x44: return {std::get<std::uint64_t>(instruction.immediate)};
    case 0x45: return {a32 == 0};
    case 0x50: return {lhs == 0};
    case 0xa7: return {a32}; // wrap_i64
    case 0xac: return {sign_extend(lhs, 32)};
    case 0xad: return {a32};
    case 0xb6: return {demote_float(lhs)};
    case 0xbb: return {convert_float<double>(std::bit_cast<float>(a32))};
    case 0xbc: case 0xbe: return {a32}; // 32-bit reinterpretations
    case 0xbd: case 0xbf: return {lhs}; // 64-bit reinterpretations
    case 0xc0: return {sign_extend(a32, 8)};
    case 0xc1: return {sign_extend(a32, 16)};
    case 0xc2: return {sign_extend(lhs, 8)};
    case 0xc3: return {sign_extend(lhs, 16)};
    case 0xc4: return {sign_extend(lhs, 32)};
    default: break;
  }
  if (byte >= 0x46 && byte <= 0x4f)
    return {integer_compare(byte - 0x46, a32, b32)};
  if (byte >= 0x51 && byte <= 0x5a)
    return {integer_compare(byte - 0x51, lhs, rhs)};
  if (byte >= 0x5b && byte <= 0x60)
    return {float_compare<float>(byte - 0x5b, a32, b32)};
  if (byte >= 0x61 && byte <= 0x66)
    return {float_compare<double>(byte - 0x61, lhs, rhs)};
  if (byte >= 0x67 && byte <= 0x69)
    return {integer_count(byte - 0x67, a32)};
  if (byte >= 0x6a && byte <= 0x78)
    return integer_binary(byte - 0x6a, a32, b32);
  if (byte >= 0x79 && byte <= 0x7b)
    return {integer_count(byte - 0x79, lhs)};
  if (byte >= 0x7c && byte <= 0x8a)
    return integer_binary(byte - 0x7c, lhs, rhs);
  if (byte >= 0x8b && byte <= 0x91)
    return {float_unary<float>(byte - 0x8b, a32)};
  if (byte >= 0x92 && byte <= 0x98)
    return {float_binary<float>(byte - 0x92, a32, b32)};
  if (byte >= 0x99 && byte <= 0x9f)
    return {float_unary<double>(byte - 0x99, lhs)};
  if (byte >= 0xa0 && byte <= 0xa6)
    return {float_binary<double>(byte - 0xa0, lhs, rhs)};
  if (byte >= 0xa8 && byte <= 0xab)
    return float_to_integer(byte - 0xa8, lhs, false);
  if (byte >= 0xae && byte <= 0xb1)
    return float_to_integer(byte - 0xae + 4, lhs, false);
  if (byte >= 0xb2 && byte <= 0xb5)
    return {integer_to_float<float>(byte - 0xb2, lhs)};
  if (byte >= 0xb7 && byte <= 0xba)
    return {integer_to_float<double>(byte - 0xb7, lhs)};
  throw std::logic_error("not a Wasm scalar numeric operation");
}

struct NumericShape {
  Type output;
  unsigned arity;
};

std::optional<NumericShape> numeric_shape(Opcode opcode) {
  const auto byte = opcode.byte;
  if (byte == 0xfc && opcode.extension < 8)
    return NumericShape{opcode.extension < 4 ? Type::i32 : Type::i64, 1};
  if (opcode.extension != 0 || byte < 0x41 || byte > 0xc4)
    return std::nullopt;
  constexpr std::array constants{Type::i32, Type::i64, Type::f32, Type::f64};
  if (byte <= 0x44)
    return NumericShape{constants[byte - 0x41], 0};
  if (byte <= 0x66)
    return NumericShape{Type::i32, byte == 0x45 || byte == 0x50 ? 1u : 2u};
  if (byte <= 0x78)
    return NumericShape{Type::i32, byte <= 0x69 ? 1u : 2u};
  if (byte <= 0x8a)
    return NumericShape{Type::i64, byte <= 0x7b ? 1u : 2u};
  if (byte <= 0x98)
    return NumericShape{Type::f32, byte <= 0x91 ? 1u : 2u};
  if (byte <= 0xa6)
    return NumericShape{Type::f64, byte <= 0x9f ? 1u : 2u};
  constexpr std::array conversion_outputs{
    Type::i32, Type::i32, Type::i32, Type::i32, Type::i32, // 0xa7–0xab
    Type::i64, Type::i64, Type::i64, Type::i64, Type::i64, Type::i64,
    Type::f32, Type::f32, Type::f32, Type::f32, Type::f32,
    Type::f64, Type::f64, Type::f64, Type::f64, Type::f64,
    Type::i32, Type::i64, Type::f32, Type::f64, // 0xbc–0xbf reinterpret
    Type::i32, Type::i32, Type::i64, Type::i64, Type::i64, // 0xc0–0xc4 sign-extend
  };
  return NumericShape{conversion_outputs[byte - 0xa7], 1};
}

bool execute_numeric(const Instruction& instruction, std::vector<Value>& stack) {
  const auto shape = numeric_shape(instruction.opcode);
  if (!shape)
    return false;
  const auto base = stack.size() - shape->arity;
  const auto lhs = shape->arity == 0 ? 0 : stack[base].bits;
  const auto rhs = shape->arity == 2 ? stack[base + 1].bits : 0;
  const auto result = evaluate_numeric(instruction, lhs, rhs);
  switch (result.fault) {
    case NumericFault::none: break;
    case NumericFault::divide_by_zero: throw Trap(instruction.offset, "integer divide by zero");
    case NumericFault::overflow: throw Trap(instruction.offset, "integer overflow");
    case NumericFault::invalid_conversion:
      throw Trap(instruction.offset, "invalid conversion to integer");
  }
  stack.resize(base + 1);
  stack.back() = {shape->output, result.bits};
  return true;
}

struct MemoryAccess {
  Type type;
  unsigned width; // In bytes.
  bool sign_extend = false;
};

// All unprefixed load and store opcodes, in order from 0x28 through 0x3e.
constexpr std::array memory_accesses{
  MemoryAccess{Type::i32, 4}, MemoryAccess{Type::i64, 8},
  MemoryAccess{Type::f32, 4}, MemoryAccess{Type::f64, 8},
  MemoryAccess{Type::i32, 1, true}, MemoryAccess{Type::i32, 1},
  MemoryAccess{Type::i32, 2, true}, MemoryAccess{Type::i32, 2},
  MemoryAccess{Type::i64, 1, true}, MemoryAccess{Type::i64, 1},
  MemoryAccess{Type::i64, 2, true}, MemoryAccess{Type::i64, 2},
  MemoryAccess{Type::i64, 4, true}, MemoryAccess{Type::i64, 4},
  MemoryAccess{Type::i32, 4}, MemoryAccess{Type::i64, 8},
  MemoryAccess{Type::f32, 4}, MemoryAccess{Type::f64, 8},
  MemoryAccess{Type::i32, 1}, MemoryAccess{Type::i32, 2},
  MemoryAccess{Type::i64, 1}, MemoryAccess{Type::i64, 2}, MemoryAccess{Type::i64, 4},
};

constexpr std::uint64_t page_size = 65'536;
constexpr std::uint32_t wasm32_max_pages = 65'536;
constexpr auto grow_failed = std::numeric_limits<std::uint32_t>::max();
// Dense backing stores need limits below host allocation overcommit. Table
// entries are bounded across all tables, not once per table.
constexpr std::uint32_t max_instance_pages = 1024; // 64 MiB.
constexpr std::uint64_t max_instance_table_entries = 1'048'576;

bool in_bounds(std::size_t size, std::uint64_t start, std::uint64_t count) {
  // A zero-length access allows start == size but not start > size.
  return start <= size && count <= size - start;
}

std::uint64_t load_little_endian(std::span<const std::uint8_t> bytes) {
  std::uint64_t bits = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bits |= std::uint64_t{bytes[i]} << (i * 8);
  return bits;
}

void store_little_endian(std::span<std::uint8_t> bytes, std::uint64_t bits) {
  for (auto& byte : bytes) {
    byte = static_cast<std::uint8_t>(bits);
    bits >>= 8;
  }
}

Value pop_value(std::vector<Value>& stack) {
  const auto value = stack.back();
  stack.pop_back();
  return value;
}

std::uint32_t pop_i32(std::vector<Value>& stack) {
  return static_cast<std::uint32_t>(pop_value(stack).bits);
}

struct RangeArguments {
  std::uint32_t dst, src, count;
};

RangeArguments pop_range(std::vector<Value>& stack) {
  const auto count = pop_i32(stack);
  const auto src = pop_i32(stack);
  const auto dst = pop_i32(stack);
  return {dst, src, count};
}

void memory_load_store(const Instruction& instruction, std::vector<Value>& stack,
                       std::span<std::uint8_t> bytes) {
  const auto byte = instruction.opcode.byte;
  const bool store = byte >= 0x36;
  const auto access = memory_accesses[byte - 0x28];
  const auto stored_bits = store ? pop_value(stack).bits : 0;
  const auto displacement = std::get<MemoryArg>(instruction.immediate).offset;
  // Both components are unsigned wasm32 values. Do not wrap before checking.
  const auto address = std::uint64_t{pop_i32(stack)} + displacement;
  if (!in_bounds(bytes.size(), address, access.width))
    throw Trap(instruction.offset, "out-of-bounds memory access");
  const auto range = bytes.subspan(static_cast<std::size_t>(address), access.width);
  if (store) {
    store_little_endian(range, stored_bits);
    return;
  }
  auto bits = load_little_endian(range);
  if (access.sign_extend)
    bits = sign_extend(bits, access.width * 8);
  if (access.type == Type::i32)
    bits = static_cast<std::uint32_t>(bits);
  stack.push_back({access.type, bits});
}

void memory_init(std::span<std::uint8_t> memory, std::span<const std::uint8_t> data,
                 RangeArguments args, std::size_t offset) {
  if (!in_bounds(memory.size(), args.dst, args.count) ||
      !in_bounds(data.size(), args.src, args.count))
    throw Trap(offset, "out-of-bounds memory.init");
  if (args.count != 0)
    std::memcpy(memory.data() + args.dst, data.data() + args.src, args.count);
}

void memory_copy(std::span<std::uint8_t> memory, RangeArguments args, std::size_t offset) {
  if (!in_bounds(memory.size(), args.dst, args.count) ||
      !in_bounds(memory.size(), args.src, args.count))
    throw Trap(offset, "out-of-bounds memory.copy");
  if (args.count != 0)
    std::memmove(memory.data() + args.dst, memory.data() + args.src, args.count);
}

void memory_fill(std::span<std::uint8_t> memory, RangeArguments args, std::size_t offset) {
  if (!in_bounds(memory.size(), args.dst, args.count))
    throw Trap(offset, "out-of-bounds memory.fill");
  if (args.count != 0)
    std::fill_n(memory.data() + args.dst, args.count, static_cast<std::uint8_t>(args.src));
}

template<class Element>
bool try_resize(std::vector<Element>& elements, std::uint64_t size, Element value) {
  if (size > elements.max_size())
    return false;
  try {
    elements.resize(static_cast<std::size_t>(size), value);
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }
}

void table_copy(std::span<Value> destination, std::span<const Value> source,
                RangeArguments args, std::size_t offset, const char* error) {
  if (!in_bounds(destination.size(), args.dst, args.count) ||
      !in_bounds(source.size(), args.src, args.count))
    throw Trap(offset, error);
  if (args.count == 0)
    return;
  const auto from = source.subspan(args.src, args.count);
  const auto to = destination.subspan(args.dst, args.count);
  if (from.data() == to.data())
    return;
  if (destination.data() == source.data() && args.dst > args.src &&
      args.dst - args.src < args.count)
    std::ranges::copy_backward(from, to.end());
  else
    std::ranges::copy(from, to.begin());
}

void table_fill(std::span<Value> table, std::vector<Value>& stack, std::size_t offset) {
  const auto count = pop_i32(stack);
  const auto value = pop_value(stack);
  const auto dst = pop_i32(stack);
  if (!in_bounds(table.size(), dst, count))
    throw Trap(offset, "out-of-bounds table.fill");
  if (count != 0)
    std::fill_n(table.data() + dst, count, value);
}

Value null_reference(const Instruction& instruction) {
  return {std::get<Type>(instruction.immediate), 0};
}

Value function_reference(const Instruction& instruction) {
  return {Type::funcref, std::uint64_t{std::get<std::uint32_t>(instruction.immediate)} + 1};
}

Value constant_expression(const Program& code, const detail::InstanceState& state) {
  const auto& instruction = code.front(); // A validated Core 2.0 init is <constant> end.
  switch (instruction.opcode.byte) {
    case 0x23: return state.globals[std::get<std::uint32_t>(instruction.immediate)]->value;
    case 0x41: return {Type::i32, std::get<std::uint32_t>(instruction.immediate)};
    case 0x42: return {Type::i64, std::get<std::uint64_t>(instruction.immediate)};
    case 0x43: return {Type::f32, std::get<std::uint32_t>(instruction.immediate)};
    case 0x44: return {Type::f64, std::get<std::uint64_t>(instruction.immediate)};
    case 0xd0: return null_reference(instruction);
    case 0xd2: return function_reference(instruction);
    case 0xfd: {
      if (instruction.opcode.extension != 0x0c)
        break;
      const auto bytes = std::span(std::get<VectorBytes>(instruction.immediate));
      return {Type::v128, load_little_endian(bytes.first<8>()),
              load_little_endian(bytes.last<8>())};
    }
    default: break;
  }
  throw Error(instruction.offset, "unsupported runtime constant expression");
}

} // namespace

namespace detail {

bool match_value(Type type, const Value& value, std::size_t function_count) {
  if (value.type != type || (type != Type::v128 && value.high != 0))
    return false;
  switch (type) {
    case Type::i32:
    case Type::f32: return value.bits <= std::numeric_limits<std::uint32_t>::max();
    case Type::funcref: return value.bits <= function_count;
    case Type::i64:
    case Type::f64:
    case Type::v128:
    case Type::externref: return true;
    default: return false;
  }
}

MemoryInstance::MemoryInstance(MemoryType type) : max_pages(type.limits.max) {
  if (type.limits.min > max_pages.value_or(wasm32_max_pages) ||
      max_pages.value_or(0) > wasm32_max_pages)
    throw Error(0, "invalid memory limits");
  if (grow(type.limits.min) == grow_failed)
    throw Error(0, "initial memory allocation exceeds instance limit or available storage");
}

MemoryType MemoryInstance::type() const noexcept {
  return {{static_cast<std::uint32_t>(bytes.size() / page_size), max_pages}};
}

std::uint32_t MemoryInstance::grow(std::uint32_t delta) {
  const auto previous = type().limits.min;
  const auto next = std::uint64_t{previous} + delta;
  if (next > std::min(max_pages.value_or(wasm32_max_pages), max_instance_pages) ||
      !try_resize(bytes, next * page_size, std::uint8_t{0}))
    return grow_failed;
  return previous;
}

TableInstance::TableInstance(TableType type) : element(type.element), max_entries(type.limits.max) {
  if (element != Type::funcref && element != Type::externref)
    throw Error(0, "invalid table element type");
  if (type.limits.min > max_entries.value_or(grow_failed))
    throw Error(0, "invalid table limits");
  if (grow(type.limits.min, {element, 0}) == grow_failed)
    throw Error(0, "initial table allocation exceeds instance limit or available storage");
}

TableType TableInstance::type() const noexcept {
  return {element, {static_cast<std::uint32_t>(entries.size()), max_entries}};
}

std::uint32_t TableInstance::grow(std::uint32_t delta, Value value) {
  const auto previous = static_cast<std::uint32_t>(entries.size());
  const auto next = std::uint64_t{previous} + delta;
  if (next > max_entries.value_or(grow_failed) || next > max_instance_table_entries)
    return grow_failed;
  // The size changes in *every* consuming instance. Check all budgets before
  // mutating the table, and never charge twice for aliased table indices.
  for (const auto* consumer : consumers)
    if (delta > max_instance_table_entries - consumer->table_entries)
      return grow_failed;
  if (!try_resize(entries, next, value))
    return grow_failed;
  for (auto* consumer : consumers)
    consumer->table_entries += delta;
  return previous;
}

InstanceState::~InstanceState() {
  for (const auto& table : tables) {
    const auto consumer = std::ranges::find(table->consumers, this);
    if (consumer == table->consumers.end())
      continue;
    table->consumers.erase(consumer);
    // A locally allocated table cannot escape the state. Imported tables
    // with no remaining handles also need no cleanup before being freed.
    if (table->element == Type::funcref && table.use_count() > 1)
      std::ranges::fill(table->entries, Value{Type::funcref, 0});
  }
  for (const auto& global : globals)
    if (global->owner == this) {
      global->value = {Type::funcref, 0};
      global->owner = nullptr;
    }
}

std::span<std::uint8_t> InstanceState::memory_bytes() const noexcept {
  return memory ? std::span(memory->bytes) : std::span<std::uint8_t>{};
}

void InstanceState::attach_table(const std::shared_ptr<TableInstance>& table, std::string_view name) {
  const bool alias = std::ranges::find(table->consumers, this) != table->consumers.end();
  if (!alias) {
    if (table->element == Type::funcref && !table->consumers.empty())
      throw Error(0, "host funcref table already bound to another instance: " + std::string(name));
    if (table->entries.size() > max_instance_table_entries - table_entries)
      throw Error(0, "initial table allocation exceeds instance limit: " + std::string(name));
  }
  tables.push_back(table); // Retain before registering so cleanup also works on failure.
  if (!alias) {
    table->consumers.push_back(this);
    table_entries += table->entries.size();
  }
}

void InstanceState::attach_global(const std::shared_ptr<GlobalInstance>& global, std::string_view name) {
  if (global->owner && global->owner != this)
    throw Error(0, "host funcref global already bound to another instance: " + std::string(name));
  globals.push_back(global);
  if (global->type.type == Type::funcref && global->type.mutable_)
    global->owner = this;
}

std::uint64_t evaluate_scalar(Opcode opcode, std::uint64_t lhs, std::uint64_t rhs) {
  const auto result = evaluate_numeric({opcode, {}, 0}, lhs, rhs);
  if (result.fault != NumericFault::none)
    throw std::logic_error("trapping Wasm scalar operation used in SIMD");
  return result.bits;
}

void initialize_state(const Module& module, InstanceState& state) {
  if (!module.memories.empty())
    state.memory = std::make_shared<MemoryInstance>(module.memories.front());

  state.tables.reserve(state.tables.size() + module.tables.size());
  for (const auto& table : module.tables) {
    if (table.limits.min > max_instance_table_entries - state.table_entries)
      throw Error(0, "initial table allocation exceeds instance limit or available storage");
    state.attach_table(std::make_shared<TableInstance>(table), {});
  }

  state.globals.reserve(state.globals.size() + module.globals.size());
  for (const auto& global : module.globals)
    state.globals.push_back(
        std::make_shared<GlobalInstance>(GlobalInstance{global.type, constant_expression(global.init, state)}));

  state.elements.resize(module.elements.size());
  for (std::size_t i = 0; i < module.elements.size(); ++i) {
    const auto& element = module.elements[i];
    if (element.mode == SegmentMode::declarative)
      continue;
    if (element.mode == SegmentMode::passive) {
      auto& values = state.elements[i];
      values.reserve(element.init.size());
      for (const auto& expression : element.init)
        values.push_back(constant_expression(expression, state));
      continue;
    }
    auto& table = state.tables[element.table]->entries;
    const auto dst = static_cast<std::uint32_t>(constant_expression(element.offset, state).bits);
    if (!in_bounds(table.size(), dst, element.init.size()))
      throw Trap(element.offset.front().offset, "out-of-bounds active element segment");
    for (std::size_t j = 0; j < element.init.size(); ++j)
      table[dst + j] = constant_expression(element.init[j], state);
  }

  state.data.resize(module.data.size());
  for (std::size_t i = 0; i < module.data.size(); ++i) {
    const auto& data = module.data[i];
    if (data.mode == SegmentMode::passive) {
      state.data[i] = data.bytes;
      continue;
    }
    const auto bytes = state.memory_bytes();
    const auto dst = static_cast<std::uint32_t>(constant_expression(data.offset, state).bits);
    if (!in_bounds(bytes.size(), dst, data.bytes.size()))
      throw Trap(data.offset.front().offset, "out-of-bounds active data segment");
    if (!data.bytes.empty())
      std::memcpy(bytes.data() + dst, data.bytes.data(), data.bytes.size());
  }
}

bool execute_opcode(const Instruction& instruction, std::vector<Value>& stack, InstanceState& state) {
  const auto [byte, extension] = instruction.opcode;
  const auto offset = instruction.offset;
  const auto index = [&]() { return std::get<std::uint32_t>(instruction.immediate); };
  const auto index_pair = [&]() { return std::get<IndexPair>(instruction.immediate); };
  const auto push_i32 = [&](std::uint32_t bits) { stack.push_back({Type::i32, bits}); };
  if (execute_numeric(instruction, stack))
    return true;
  if (byte >= 0x28 && byte <= 0x3e) {
    memory_load_store(instruction, stack, state.memory_bytes());
    return true;
  }
  switch (byte) {
    case 0x23: // global.get
      stack.push_back(state.globals[index()]->value);
      return true;
    case 0x24: // global.set
      state.globals[index()]->value = pop_value(stack);
      return true;
    case 0x25: { // table.get
      const auto& table = state.tables[index()]->entries;
      const auto entry = pop_i32(stack);
      if (entry >= table.size())
        throw Trap(offset, "out-of-bounds table.get");
      stack.push_back(table[entry]);
      return true;
    }
    case 0x26: { // table.set
      auto& table = state.tables[index()]->entries;
      const auto value = pop_value(stack);
      const auto entry = pop_i32(stack);
      if (entry >= table.size())
        throw Trap(offset, "out-of-bounds table.set");
      table[entry] = value;
      return true;
    }
    case 0x3f: // memory.size
      push_i32(state.memory->type().limits.min);
      return true;
    case 0x40: // memory.grow
      push_i32(state.memory->grow(pop_i32(stack)));
      return true;
    case 0xd0: // ref.null
      stack.push_back(null_reference(instruction));
      return true;
    case 0xd1: // ref.is_null
      stack.back() = {Type::i32, stack.back().bits == 0};
      return true;
    case 0xd2: // ref.func
      stack.push_back(function_reference(instruction));
      return true;
    case 0xfd:
      return execute_simd(instruction, stack, state.memory_bytes());
    case 0xfc:
      break;
    default:
      return false;
  }
  switch (extension) {
    case 8: // memory.init
      memory_init(state.memory_bytes(), state.data[index()], pop_range(stack), offset);
      return true;
    case 9: // data.drop
      state.data[index()] = {};
      return true;
    case 10: // memory.copy
      memory_copy(state.memory_bytes(), pop_range(stack), offset);
      return true;
    case 11: // memory.fill
      memory_fill(state.memory_bytes(), pop_range(stack), offset);
      return true;
    case 12: { // table.init
      const auto [element, table] = index_pair();
      table_copy(state.tables[table]->entries, state.elements[element], pop_range(stack), offset,
                 "out-of-bounds table.init");
      return true;
    }
    case 13: // elem.drop
      std::vector<Value>{}.swap(state.elements[index()]);
      return true;
    case 14: { // table.copy
      const auto [dst, src] = index_pair();
      table_copy(state.tables[dst]->entries, state.tables[src]->entries, pop_range(stack), offset,
                 "out-of-bounds table.copy");
      return true;
    }
    case 15: { // table.grow
      const auto delta = pop_i32(stack);
      const auto value = pop_value(stack);
      push_i32(state.tables[index()]->grow(delta, value));
      return true;
    }
    case 16: // table.size
      push_i32(state.tables[index()]->type().limits.min);
      return true;
    case 17: // table.fill
      table_fill(state.tables[index()]->entries, stack, offset);
      return true;
    default:
      return false;
  }
}

} // namespace detail
} // namespace limestone::wasmutils::core
