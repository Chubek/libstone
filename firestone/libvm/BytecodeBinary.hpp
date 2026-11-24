#pragma once
#include "Module.hpp"
#include <cstring>
#include <string_view>

namespace firestone::vm {

using BytecodeBytes = std::vector<std::uint8_t>;
struct BinaryLimits {
    Index bytes = 64 * 1024 * 1024, items = 1000000, string_bytes = 1024 * 1024;
};
namespace binary_detail {
// A constant encoding tag, not a signature category.
inline constexpr std::uint8_t wide_integer_tag = 7;

inline std::string format_hex_integer(const BigInt& value) {
    if (value.is_zero()) return "0";
    const auto bits = value.bit_length();
    const auto digits = (bits - 1) / 4 + 1;
    std::string text(digits + value.negative(), '0');
    if (value.negative()) text.front() = '-';
    for (Index digit = 0; digit < digits; ++digit) {
        unsigned nibble = 0;
        for (unsigned bit = 0; bit < 4; ++bit)
            if (value.test_bit(digit * 4 + bit)) nibble |= 1u << bit;
        text[text.size() - digit - 1] = "0123456789abcdef"[nibble];
    }
    return text;
}

// Combine bounded hexadecimal chunks rather than repeatedly shifting a
// growing integer one digit at a time on untrusted bytecode input.
inline BigInt parse_hex_magnitude(std::string_view digits) {
    if (digits.size() <= 15) return BigInt(digits, 16);
    const auto middle = digits.size() / 2;
    auto low = digits.substr(middle);
    return (parse_hex_magnitude(digits.substr(0, middle)) << (4 * low.size())) + parse_hex_magnitude(low);
}

struct Writer {
    BytecodeBytes bytes;
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u64(std::uint64_t value) { for (unsigned n = 0; n < 8; ++n) u8(static_cast<std::uint8_t>(value >> (n * 8))); }
    void index(Index value) { u64(value == no_index ? UINT64_MAX : static_cast<std::uint64_t>(value)); }
    void string(const std::string& value) { index(value.size()); bytes.insert(bytes.end(), value.begin(), value.end()); }
    void signature(const Signature& value) {
        u8(static_cast<std::uint8_t>(value.result)); index(value.parameters.size());
        for (auto parameter : value.parameters) u8(static_cast<std::uint8_t>(parameter));
    }
};
struct Reader {
    const BytecodeBytes& bytes;
    BinaryLimits limits;
    Index position = 0, items = 0;
    [[noreturn]] void fail(const char* message) const { throw std::invalid_argument(std::string("bytecode binary: ") + message); }
    std::uint8_t u8() { if (position == bytes.size()) fail("truncated input"); return bytes[position++]; }
    std::uint64_t u64() { std::uint64_t result = 0; for (unsigned n = 0; n < 8; ++n) result |= std::uint64_t{u8()} << (n * 8); return result; }
    Index index() {
        auto value = u64();
        if (value == UINT64_MAX) return no_index;
        if (value >= static_cast<std::uint64_t>(no_index)) fail("index exceeds host capacity");
        return static_cast<Index>(value);
    }
    Index count() {
        auto value = index();
        if (value > limits.items - items || value > bytes.size() - position) fail("item budget exceeded");
        items += value; return value;
    }
    std::string string() {
        auto size = index();
        if (size > limits.string_bytes || size > bytes.size() - position) fail("invalid string length");
        auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(position);
        std::string result(begin, begin + static_cast<std::ptrdiff_t>(size)); position += size; return result;
    }
    ValueKind value_kind() {
        auto value = static_cast<ValueKind>(u8()); if (!valid_kind(value)) fail("invalid value kind"); return value;
    }
    Signature signature() {
        Signature result; result.result = value_kind(); auto size = count();
        for (Index n = 0; n < size; ++n) result.parameters.push_back(value_kind());
        return result;
    }
};
} // namespace binary_detail

// FSVM v1: fixed-width little-endian fields; IEEE-754 binary64 floats.
// FSVM v2 adds length-prefixed signed hexadecimal constants and permits
// dynamic-width scalar instructions. Signatures and record layouts are unchanged.
inline BytecodeBytes encode(const Module& module) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "bytecode requires IEEE binary64");
    require_valid(module);
    binary_detail::Writer writer;
    for (auto byte : std::string("FSVM")) writer.u8(static_cast<std::uint8_t>(byte));
    const auto& constants = module.constants.values();
    const auto wide = [](const Value& value) {
        const auto* number = std::get_if<BigInt>(&value);
        return number && !value_detail::fits_int64(*number);
    };
    bool needs_v2 = false;
    for (const auto& value : constants) if (wide(value)) { needs_v2 = true; break; }
    for (Index f = 0; !needs_v2 && f < module.size(); ++f)
        for (const auto& i : module.function(f).code.instructions())
            if (i.bits == dynamic_scalar_width) { needs_v2 = true; break; }
    writer.u64(needs_v2 ? 2 : 1); writer.index(module.constants.size());
    for (const auto& value : constants) {
        if (wide(value)) {
            writer.u8(binary_detail::wide_integer_tag);
            writer.string(binary_detail::format_hex_integer(require<BigInt>(value)));
            continue;
        }
        writer.u8(static_cast<std::uint8_t>(kind(value)));
        switch (kind(value)) {
        case ValueKind::Null: break;
        case ValueKind::Boolean: writer.u8(require<bool>(value) ? 1 : 0); break;
        case ValueKind::Integer: writer.u64(static_cast<std::uint64_t>(require_integer64(value))); break;
        case ValueKind::Float: {
            std::uint64_t bits; auto number = require<double>(value); std::memcpy(&bits, &number, 8); writer.u64(bits); break;
        }
        case ValueKind::String: writer.string(require<std::string>(value)); break;
        default: throw std::invalid_argument("nonportable constant kind");
        }
    }
    writer.index(module.natives.size());
    for (const auto& native : module.natives) { writer.string(native.first); writer.signature(native.second); }
    writer.index(module.size());
    for (Index f = 0; f < module.size(); ++f) {
        const auto& fn = module.function(f); writer.string(fn.name);
        writer.u8(static_cast<std::uint8_t>(fn.model)); writer.signature(fn.signature);
        writer.index(fn.register_count); writer.index(fn.local_count); writer.index(fn.code.size());
        for (const auto& i : fn.code.instructions()) {
            writer.u8(static_cast<std::uint8_t>(i.opcode)); writer.index(i.destination);
            writer.index(i.operands.size()); for (auto operand : i.operands) writer.index(operand);
            writer.index(i.index); writer.index(i.target); writer.index(i.alternate);
            writer.index(i.argument_count); writer.u8(static_cast<std::uint8_t>(i.bits)); writer.string(i.symbol);
        }
    }
    return std::move(writer.bytes);
}
inline Module decode(const BytecodeBytes& bytes, const BinaryLimits& limits = {}) {
    if (bytes.size() > limits.bytes) throw std::invalid_argument("bytecode binary: byte budget exceeded");
    binary_detail::Reader reader{bytes, limits};
    for (auto byte : std::string("FSVM")) if (reader.u8() != static_cast<std::uint8_t>(byte)) reader.fail("bad magic");
    const auto version = reader.u64();
    if (version != 1 && version != 2) reader.fail("unsupported version");
    Module module; auto constants = reader.count();
    bool needs_v2 = false;
    for (Index n = 0; n < constants; ++n) {
        const auto encoding = reader.u8();
        if (version == 2 && encoding == binary_detail::wide_integer_tag) {
            const auto text = reader.string();
            std::string_view digits = text;
            const bool negative = !digits.empty() && digits.front() == '-';
            if (negative) digits.remove_prefix(1);
            if (digits.empty() || digits.front() == '0' ||
                digits.find_first_not_of("0123456789abcdef") != std::string_view::npos)
                reader.fail("invalid wide integer");
            if (digits.size() > std::numeric_limits<Index>::max() / 4) reader.fail("wide integer exceeds host capacity");
            BigInt number;
            try { number = binary_detail::parse_hex_magnitude(digits); }
            catch (const std::length_error&) { reader.fail("wide integer exceeds host capacity"); }
            if (negative) number = -number;
            if (value_detail::fits_int64(number)) reader.fail("noncanonical wide integer");
            if (module.constants.add(Value(std::move(number))) != n) reader.fail("duplicate canonical constant");
            needs_v2 = true;
            continue;
        }
        auto tag = static_cast<ValueKind>(encoding); Value value;
        switch (tag) {
        case ValueKind::Null: break;
        case ValueKind::Boolean: { auto boolean = reader.u8(); if (boolean > 1) reader.fail("invalid boolean"); value = boolean != 0; break; }
        case ValueKind::Integer: value = signed_bits(reader.u64()); break;
        case ValueKind::Float: { auto bits = reader.u64(); double number; std::memcpy(&number, &bits, 8); value = number; break; }
        case ValueKind::String: value = reader.string(); break;
        default: reader.fail("nonportable constant kind");
        }
        if (module.constants.add(std::move(value)) != n) reader.fail("duplicate canonical constant");
    }
    auto natives = reader.count();
    for (Index n = 0; n < natives; ++n) {
        auto name = reader.string(); auto signature = reader.signature();
        if (!module.natives.emplace(std::move(name), std::move(signature)).second) reader.fail("duplicate native declaration");
    }
    auto functions = reader.count();
    for (Index f = 0; f < functions; ++f) {
        Function fn; fn.name = reader.string(); fn.model = static_cast<VMModel>(reader.u8()); fn.signature = reader.signature();
        fn.register_count = reader.index(); fn.local_count = reader.index(); auto instructions = reader.count();
        if (fn.register_count > limits.items || fn.local_count > limits.items) reader.fail("frame slot budget exceeded");
        for (Index pc = 0; pc < instructions; ++pc) {
            Instruction i; i.opcode = static_cast<Opcode>(reader.u8()); i.destination = reader.index();
            auto operands = reader.count(); for (Index n = 0; n < operands; ++n) i.operands.push_back(reader.index());
            i.index = reader.index(); i.target = reader.index(); i.alternate = reader.index();
            i.argument_count = reader.index(); i.bits = reader.u8(); i.symbol = reader.string();
            if (i.bits == dynamic_scalar_width) {
                if (version == 1) reader.fail("dynamic scalar width requires version 2");
                needs_v2 = true;
            }
            fn.code.emit(std::move(i));
        }
        module.add_function(std::move(fn));
    }
    if (reader.position != bytes.size()) reader.fail("trailing input");
    if (version == 2 && !needs_v2) reader.fail("no version 2 features");
    auto errors = module.verify(limits.items);
    if (!errors.empty()) throw std::invalid_argument(errors.front().code + ": " + errors.front().message);
    return module;
}

} // namespace firestone::vm
