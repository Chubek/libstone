#ifndef SANDSTONE_ARCHML_RUNTIME_HPP
#define SANDSTONE_ARCHML_RUNTIME_HPP

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sandstone::archml::runtime {

// Signed operands use their uint64_t two's-complement representation. This also
// allows unsigned 64-bit immediates without narrowing through int64_t.
struct Operand {
    std::string name, kind;
    unsigned bits = 0;
    bool is_signed = false;
    std::uint64_t scale = 1;
    std::vector<std::uint64_t> excluded, registers;
};
struct Field {
    std::size_t operand = 0, offset = 0;
    unsigned width = 0, shift = 0;
};
struct Instruction {
    std::string name, assembly;
    std::vector<Operand> operands;
    std::vector<std::uint8_t> base;
    std::vector<Field> fields;
    std::vector<std::string> features, modes;
};
struct Decoded {
    std::size_t instruction = 0, size = 0;
    std::vector<std::uint64_t> operands;
};

inline std::uint64_t mask(unsigned bits) {
    return bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
}
inline bool normalize(const Operand &op, std::uint64_t value, std::uint64_t &raw) {
    if (op.bits == 0 || op.bits > 64 || op.scale == 0 || op.scale > INT64_MAX ||
        op.kind == "value" ||
        std::find(op.excluded.begin(), op.excluded.end(), value) != op.excluded.end())
        return false;
    if (op.kind == "register" &&
        std::find(op.registers.begin(), op.registers.end(), value) == op.registers.end())
        return false;
    if (op.is_signed) {
        const auto signed_value = std::bit_cast<std::int64_t>(value);
        const auto scale = static_cast<std::int64_t>(op.scale);
        if (signed_value % scale != 0) return false;
        const auto quotient = signed_value / scale;
        if (op.bits < 64) {
            const auto bound = INT64_C(1) << (op.bits - 1);
            if (quotient < -bound || quotient >= bound) return false;
        }
        raw = static_cast<std::uint64_t>(quotient) & mask(op.bits);
    } else {
        if (value % op.scale != 0) return false;
        raw = value / op.scale;
        if (raw > mask(op.bits)) return false;
    }
    return true;
}
inline std::vector<std::uint8_t> encode(const Instruction &instruction,
                                       std::span<const std::uint64_t> values) {
    if (instruction.base.empty()) throw std::invalid_argument("operation has no binary encoding");
    if (values.size() != instruction.operands.size())
        throw std::invalid_argument("incorrect operand count for " + instruction.name);
    std::vector<std::uint64_t> raw(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        if (!normalize(instruction.operands[i], values[i], raw[i]))
            throw std::invalid_argument("invalid operand " + instruction.operands[i].name +
                                        " for " + instruction.name);
    auto bytes = instruction.base;
    for (const auto &field : instruction.fields)
        for (unsigned bit = 0; bit < field.width; ++bit) {
            const auto at = field.offset + bit;
            const auto flag = static_cast<std::uint8_t>(1u << (at % 8));
            bytes[at / 8] = static_cast<std::uint8_t>(
                (bytes[at / 8] & ~flag) | (((raw[field.operand] >> (field.shift + bit)) & 1) ? flag : 0));
        }
    return bytes;
}
inline bool decode_one(const Instruction &instruction, std::span<const std::uint8_t> bytes,
                       std::vector<std::uint64_t> &values) {
    if (instruction.base.empty() || bytes.size() < instruction.base.size()) return false;
    auto fixed = std::vector<std::uint8_t>(instruction.base.size(), 255);
    std::vector<std::uint64_t> raw(instruction.operands.size());
    for (const auto &field : instruction.fields)
        for (unsigned bit = 0; bit < field.width; ++bit) {
            const auto at = field.offset + bit;
            fixed[at / 8] &= static_cast<std::uint8_t>(~(1u << (at % 8)));
            raw[field.operand] |= std::uint64_t((bytes[at / 8] >> (at % 8)) & 1) << (field.shift + bit);
        }
    for (std::size_t i = 0; i < fixed.size(); ++i)
        if ((bytes[i] & fixed[i]) != instruction.base[i]) return false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto &op = instruction.operands[i];
        if (op.is_signed && op.bits < 64 && (raw[i] & (UINT64_C(1) << (op.bits - 1))))
            raw[i] |= ~mask(op.bits);
        if (op.is_signed) {
            const auto value = std::bit_cast<std::int64_t>(raw[i]);
            const auto scale = static_cast<std::int64_t>(op.scale);
            if (value > INT64_MAX / scale || value < INT64_MIN / scale) return false;
        } else if (raw[i] > UINT64_MAX / op.scale) return false;
        raw[i] *= op.scale;
        std::uint64_t checked;
        if (!normalize(op, raw[i], checked)) return false;
    }
    values = std::move(raw);
    return true;
}
// Ambiguous encodings are resolved by source order. Use decode_one when a
// specific form is required; decode consumes one instruction from a byte stream.
inline bool decode(std::span<const Instruction> instructions, std::span<const std::uint8_t> bytes,
                   Decoded &result) {
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        std::vector<std::uint64_t> values;
        if (decode_one(instructions[i], bytes, values)) {
            result = {i, instructions[i].base.size(), std::move(values)};
            return true;
        }
    }
    return false;
}
inline const Instruction *find(std::span<const Instruction> instructions, std::string_view name) {
    for (const auto &instruction : instructions)
        if (instruction.name == name) return &instruction;
    return nullptr;
}
inline std::string render(const Instruction &instruction, std::span<const std::string> operands,
                          const std::map<std::string, std::string> &context = {}) {
    if (operands.size() != instruction.operands.size())
        throw std::invalid_argument("incorrect assembly operand count");
    std::string result;
    for (std::size_t i = 0; i < instruction.assembly.size();) {
        if (instruction.assembly[i] != '{') { result += instruction.assembly[i++]; continue; }
        const auto end = instruction.assembly.find('}', i);
        if (end == std::string::npos) throw std::invalid_argument("unterminated assembly placeholder");
        const auto name = instruction.assembly.substr(i + 1, end - i - 1);
        auto op = std::find_if(instruction.operands.begin(), instruction.operands.end(),
                               [&](const Operand &value) { return value.name == name; });
        if (op == instruction.operands.end()) {
            auto binding = context.find(name);
            if (binding == context.end()) throw std::invalid_argument("missing assembly binding " + name);
            result += binding->second;
        } else result += operands[static_cast<std::size_t>(op - instruction.operands.begin())];
        i = end + 1;
    }
    return result;
}
} // namespace sandstone::archml::runtime
#endif
