#pragma once

#include "WASM-IR.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace limestone::wasmutils {

struct Value {
    ValueType type;
    // An i32 is stored with its high 32 bits zero.
    std::uint64_t bits;

    bool operator==(const Value&) const = default;
};

struct Execution {
    // Bottom to top at normal completion; empty on a trap.
    std::vector<Value> stack;
    bool trapped = false;
    // The trapping instruction's source offset, or empty on success.
    std::optional<std::size_t> trap_offset;
};

// Execute a raw instruction stream from an empty stack. Invalid code throws Error
// before any instruction executes, including errors in a suffix after unreachable.
Execution execute(std::span<const Instruction> program);

} // namespace limestone::wasmutils
