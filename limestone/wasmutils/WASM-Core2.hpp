#pragma once

#include "WASM-IR.hpp"

#include <array>
#include <compare>
#include <optional>
#include <variant>

namespace limestone::wasmutils::core2 {

// The Core 2.0 instruction encoding, not a complete module validator.
struct Opcode {
    std::uint8_t byte{};
    std::uint32_t extension{}; // Only 0xfc and 0xfd take a u32 extension.
    auto operator<=>(const Opcode&) const = default;
};

enum class Type : std::uint8_t {
    i32 = 0x7f, i64 = 0x7e, f32 = 0x7d, f64 = 0x7c, v128 = 0x7b,
    funcref = 0x70, externref = 0x6f
};

struct BlockType {
    std::variant<std::monostate, Type, std::uint32_t> value;
    bool operator==(const BlockType&) const = default;
};
struct IndexPair {
    std::uint32_t first{}, second{};
    bool operator==(const IndexPair&) const = default;
};
struct BranchTable {
    std::vector<std::uint32_t> labels;
    std::uint32_t otherwise{};
    bool operator==(const BranchTable&) const = default;
};
struct MemoryArg {
    std::uint32_t align{}, offset{}; // align is the binary exponent, not bytes.
    bool operator==(const MemoryArg&) const = default;
};
struct MemoryLane {
    MemoryArg memory;
    std::uint8_t lane{};
    bool operator==(const MemoryLane&) const = default;
};
using VectorBytes = std::array<std::uint8_t, 16>;

enum class Format {
    none, block, index, index_pair, branch_table, types, ref_type,
    memory, memory_lane, lane, zero, index_zero, zero_zero,
    i32, i64, f32, f64, vector_bytes
};

// Raw bit patterns for constants (including floats); integers are not host
// signed values. Reserved zero bytes carry no operand. The two `select`
// encodings share a mnemonic and are distinguished by their format.
using Immediate = std::variant<std::monostate, BlockType, std::uint32_t, std::uint64_t,
    IndexPair, BranchTable, std::vector<Type>, Type, MemoryArg, MemoryLane,
    std::uint8_t, VectorBytes>;

struct OpcodeInfo {
    Opcode opcode;
    std::string_view mnemonic;
    Format format;
};

struct Instruction {
    Opcode opcode;
    Immediate immediate;
    std::size_t offset{}; // Source offset, ignored when encoding.
    bool operator==(const Instruction&) const = default;
};
using Program = std::vector<Instruction>;

std::span<const OpcodeInfo> opcodes() noexcept;
const OpcodeInfo* lookup(Opcode opcode) noexcept;
const OpcodeInfo* lookup(std::string_view mnemonic, bool typed_select = false) noexcept;

// Decode a flat binary stream (including else/end delimiters). Checks binary
// grammar only, not typing, index ranges, alignment, or lane bounds.
Program decode(std::span<const std::uint8_t> bytes);
// Canonicalizes LEBs and preserves floating-point/vector payload bits.
std::vector<std::uint8_t> encode(std::span<const Instruction> instructions);
// Check block/loop/if nesting and the single terminal end of a complete
// expression. This does not check stack effects, branches, or module context.
void check_expression(std::span<const Instruction> instructions);

struct LocalRun {
    std::uint32_t count{};
    Type type{};
    bool operator==(const LocalRun&) const = default;
};
struct FunctionBody {
    std::size_t offset{}; // Start of body payload (local declarations).
    std::vector<LocalRun> locals;
    Program instructions; // Instructions retain offsets relative to the module.
};
// Read the code section of a Core 2.0 binary module. Checks the module header,
// section framing, local declarations, body framing and expression nesting.
// Other sections are skipped; this is not a module validator or loader.
std::vector<FunctionBody> decode_code_section(std::span<const std::uint8_t> module);

} // namespace limestone::wasmutils::core2
