#pragma once
#include "Value.hpp"
#include "../libsema/OverflowCheck.hpp"
#include <algorithm>
#include <array>

namespace firestone::vm {

// Name, input count (-1 = instruction/signature dependent), output count.
#define FIRESTONE_VM_OPCODES(X) \
    X(Nop, 0, 0) X(Constant, 0, 1) X(Move, 1, 1) \
    X(LocalGet, 0, 1) X(LocalSet, 1, 0) X(Pop, 1, 0) X(Dup, 1, 2) X(Swap, 2, 2) \
    X(Add, 2, 1) X(Sub, 2, 1) X(Mul, 2, 1) X(Div, 2, 1) X(Mod, 2, 1) \
    X(And, 2, 1) X(Or, 2, 1) X(Xor, 2, 1) X(Shl, 2, 1) X(Shr, 2, 1) \
    X(Neg, 1, 1) X(Not, 1, 1) X(Eq, 2, 1) X(Ne, 2, 1) X(Lt, 2, 1) \
    X(Le, 2, 1) X(Gt, 2, 1) X(Ge, 2, 1) X(Select, 3, 1) \
    X(Load, 1, 1) X(Store, 2, 0) X(GetGlobal, 0, 1) X(SetGlobal, 1, 0) \
    X(GetField, 1, 1) X(SetField, 2, 0) X(Call, -1, -1) X(NativeCall, -1, -1) \
    X(Jump, 0, 0) X(Branch, 1, 0) X(Return, -1, 0)

enum class Opcode : std::uint8_t {
#define FIRESTONE_VM_ENUM(name, inputs, outputs) name,
    FIRESTONE_VM_OPCODES(FIRESTONE_VM_ENUM)
#undef FIRESTONE_VM_ENUM
    Count
};
struct OpcodeInfo { const char* name; int inputs, outputs; };
inline constexpr Index opcode_count = static_cast<Index>(Opcode::Count);
inline constexpr std::array<OpcodeInfo, opcode_count> opcode_table = {{
#define FIRESTONE_VM_INFO(name, inputs, outputs) {#name, inputs, outputs},
    FIRESTONE_VM_OPCODES(FIRESTONE_VM_INFO)
#undef FIRESTONE_VM_INFO
}};
#undef FIRESTONE_VM_OPCODES
inline bool valid_opcode(Opcode opcode) { return static_cast<Index>(opcode) < opcode_count; }
inline const OpcodeInfo& opcode_info(Opcode opcode) {
    if (!valid_opcode(opcode)) throw std::invalid_argument("invalid VM opcode");
    return opcode_table[static_cast<Index>(opcode)];
}
inline bool arithmetic(Opcode opcode) { return opcode >= Opcode::Add && opcode <= Opcode::Ge; }
inline ir::Opcode ir_opcode(Opcode opcode) {
    if (!arithmetic(opcode)) throw std::invalid_argument("not a scalar VM operation");
    return static_cast<ir::Opcode>(static_cast<int>(ir::Opcode::Add) +
        static_cast<int>(opcode) - static_cast<int>(Opcode::Add));
}
inline constexpr unsigned dynamic_scalar_width = 0;
inline std::int64_t signed_bits(std::uint64_t value) {
    const auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return value <= max ? static_cast<std::int64_t>(value) : -1 - static_cast<std::int64_t>(~value);
}
namespace detail {

inline void check_scalar_operands(Opcode opcode, const std::vector<Value>& values, unsigned bits) {
    if (!arithmetic(opcode)) throw std::invalid_argument("not a scalar VM operation");
    if (values.size() != static_cast<Index>(opcode_info(opcode).inputs)) throw VMError("vm.arity", "scalar operand count");
    if (bits > 64) throw VMError("vm.width", "scalar width must be 0 through 64");
}
inline bool scalar_comparison(Opcode opcode) { return opcode >= Opcode::Eq && opcode <= Opcode::Ge; }
inline bool scalar_shift(Opcode opcode) { return opcode == Opcode::Shl || opcode == Opcode::Shr; }

template<class T>
inline Value compare_scalar(Opcode opcode, const T& x, const T& y) {
    switch (opcode) {
    case Opcode::Eq: return x == y;
    case Opcode::Ne: return x != y;
    case Opcode::Lt: return x < y;
    case Opcode::Le: return x <= y;
    case Opcode::Gt: return x > y;
    case Opcode::Ge: return x >= y;
    default: throw VMError("vm.type", "unsupported scalar operation");
    }
}
template<class T>
inline Value equal_scalar(Opcode opcode, const T& x, const T& y) {
    switch (opcode) {
    case Opcode::Eq: return x == y;
    case Opcode::Ne: return !(x == y);
    default: throw VMError("vm.type", "unsupported scalar operation");
    }
}
inline Value apply_scalar(Opcode opcode, bool x, bool y, unsigned) {
    switch (opcode) {
    case Opcode::And: return x && y;
    case Opcode::Or: return x || y;
    case Opcode::Xor: return x != y;
    case Opcode::Not: return !x;
    default: return compare_scalar(opcode, x, y);
    }
}
inline Value apply_scalar(Opcode opcode, std::int64_t x, std::int64_t y, unsigned bits) {
    sema::SignedRange range(bits);
    if (!range.contains(x) || (!scalar_shift(opcode) && !range.contains(y)))
        throw VMError("vm.overflow", "operand exceeds signed width");
    if (scalar_comparison(opcode)) return compare_scalar(opcode, x, y);
    switch (opcode) {
    case Opcode::And: case Opcode::Or: case Opcode::Xor: case Opcode::Not: {
        auto u = static_cast<std::uint64_t>(x), v = static_cast<std::uint64_t>(y);
        auto r = opcode == Opcode::And ? u & v : opcode == Opcode::Or ? u | v : opcode == Opcode::Xor ? u ^ v : ~u;
        if (bits < 64) {
            auto mask = (std::uint64_t{1} << bits) - 1;
            r &= mask;
            if (r & (std::uint64_t{1} << (bits - 1))) r |= ~mask;
        }
        return signed_bits(r);
    }
    default: {
        auto result = sema::checked_integer(ir_opcode(opcode), x, y, bits);
        if (!result) {
            const char* code = result.error == sema::ArithmeticError::DivisionByZero ? "vm.divzero" :
                result.error == sema::ArithmeticError::InvalidShift ? "vm.shift" : "vm.overflow";
            throw VMError(code, "checked integer operation failed");
        }
        return *result.value;
    }
    }
}
inline bool bitwise_bit(Opcode opcode, bool x, bool y) {
    return opcode == Opcode::And ? x && y : opcode == Opcode::Or ? x || y : x != y;
}
inline BigInt merge_integer_bits(Opcode opcode, const BigInt& x, const BigInt& y, Index offset, Index width,
                                 bool x_fill, bool y_fill, bool result_fill) {
    if (width <= 32) {
        std::int64_t word = 0;
        for (Index i = 0; i < width; ++i) {
            const bool bit = bitwise_bit(opcode, x.test_bit(offset + i) != x_fill, y.test_bit(offset + i) != y_fill);
            if (bit != result_fill) word |= std::int64_t{1} << i;
        }
        return BigInt(word);
    }
    // Join balanced chunks rather than shifting a growing result once per bit.
    const auto low_width = width / 2;
    auto low = merge_integer_bits(opcode, x, y, offset, low_width, x_fill, y_fill, result_fill);
    auto high = merge_integer_bits(opcode, x, y, offset + low_width, width - low_width, x_fill, y_fill, result_fill);
    return (high << low_width) + low;
}
inline Value integer_bitwise(Opcode opcode, const BigInt& x, const BigInt& y) {
    const bool x_fill = x.negative(), y_fill = y.negative();
    const bool result_fill = bitwise_bit(opcode, x_fill, y_fill);
    // The sign bits of a negative integer are ones above the finite complement -x - 1.
    const BigInt x_complement = x_fill ? -x - BigInt{1} : BigInt{};
    const BigInt y_complement = y_fill ? -y - BigInt{1} : BigInt{};
    const auto& x_bits = x_fill ? x_complement : x;
    const auto& y_bits = y_fill ? y_complement : y;
    auto width = std::max(x_bits.bit_length(), y_bits.bit_length());
    try {
        auto result = merge_integer_bits(opcode, x_bits, y_bits, 0, width, x_fill, y_fill, result_fill);
        return result_fill ? -result - BigInt{1} : std::move(result);
    } catch (const std::length_error&) {
        throw VMError("vm.overflow", "integer bitwise result exceeds implementation limit");
    }
}
inline Value arbitrary_integer(Opcode opcode, const BigInt& x, const BigInt& y) {
    switch (opcode) {
    case Opcode::Add: return x + y;
    case Opcode::Sub: return x - y;
    case Opcode::Mul: return x * y;
    case Opcode::Div: case Opcode::Mod:
        if (y.is_zero()) throw VMError("vm.divzero", "integer division by zero");
        return opcode == Opcode::Div ? x / y : x % y;
    case Opcode::Neg: return -x;
    case Opcode::Not: return -x - BigInt{1};
    case Opcode::And: case Opcode::Or: case Opcode::Xor: return integer_bitwise(opcode, x, y);
    case Opcode::Shl: case Opcode::Shr: {
        if (y.negative()) throw VMError("vm.shift", "negative integer shift count");
        const auto count_bits = y.bit_length();
        if (count_bits > std::numeric_limits<Index>::digits) {
            if (opcode == Opcode::Shr) return x.negative() ? -1 : 0;
            throw VMError("vm.shift", "integer shift count exceeds host capacity");
        }
        Index count = 0;
        for (Index i = 0; i < count_bits; ++i) if (y.test_bit(i)) count |= Index{1} << i;
        try {
            return opcode == Opcode::Shl ? x << count : x >> count;
        } catch (const std::length_error&) {
            throw VMError("vm.shift", "integer shift count exceeds implementation limit");
        }
    }
    default: return compare_scalar(opcode, x, y);
    }
}
inline Value integer_scalar(Opcode opcode, const Value& a, const Value& b, unsigned bits) {
    if (bits == dynamic_scalar_width) return arbitrary_integer(opcode, require_integer(a), require_integer(b));
    auto x = require_integer64(a);
    auto y = std::int64_t{64}; // Out-of-range shift count, even at the widest fixed width.
    if (!scalar_shift(opcode)) y = require_integer64(b);
    else if (auto compact = std::get_if<std::int64_t>(&b)) y = *compact;
    else {
        const auto& wide = require<BigInt>(b);
        if (!wide.negative() && wide.bit_length() <= 6) y = wide.to_int64();
    }
    return apply_scalar(opcode, x, y, bits);
}
template<class T>
inline Value numeric_scalar(Opcode opcode, const T& x, const T& y) {
    switch (opcode) {
    case Opcode::Add: return x + y;
    case Opcode::Sub: return x - y;
    case Opcode::Mul: return x * y;
    case Opcode::Div: return x / y;
    case Opcode::Neg: return -x;
    default: return equal_scalar(opcode, x, y);
    }
}
inline Value apply_scalar(Opcode opcode, double x, double y, unsigned) {
    if (scalar_comparison(opcode)) return compare_scalar(opcode, x, y);
    return numeric_scalar(opcode, x, y);
}
inline Value apply_scalar(Opcode opcode, const std::string& x, const std::string& y, unsigned) {
    if (opcode == Opcode::Add) return x + y;
    return compare_scalar(opcode, x, y);
}
inline Value builtin_scalar(Opcode opcode, const std::vector<Value>& values, unsigned bits) {
    const auto& a = values[0];
    const auto& b = values.size() == 2 ? values[1] : a;
    auto type = kind(a);
    if (type != kind(b)) throw VMError("vm.type", "scalar operand kinds differ");
    switch (type) {
    case ValueKind::Null: return equal_scalar(opcode, std::monostate{}, std::monostate{});
    case ValueKind::Boolean: return apply_scalar(opcode, require<bool>(a), require<bool>(b), bits);
    case ValueKind::Integer: return integer_scalar(opcode, a, b, bits);
    case ValueKind::Float: return apply_scalar(opcode, require<double>(a), require<double>(b), bits);
    case ValueKind::String: return apply_scalar(opcode, require<std::string>(a), require<std::string>(b), bits);
    case ValueKind::Address: return equal_scalar(opcode, require<Address>(a), require<Address>(b));
    case ValueKind::Object: return equal_scalar(opcode, require<ObjectRef>(a), require<ObjectRef>(b));
    }
    throw VMError("vm.type", "unsupported scalar operand kind");
}

} // namespace detail

inline Value scalar(Opcode opcode, const std::vector<Value>& values, unsigned bits = 64) {
    detail::check_scalar_operands(opcode, values, bits);
    return detail::builtin_scalar(opcode, values, bits);
}

} // namespace firestone::vm
