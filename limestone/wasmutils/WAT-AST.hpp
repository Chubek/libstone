#pragma once

#include "WASM-IR.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace limestone::wasmutils {

struct WATInstruction {
    Instruction instruction; // Offset of the mnemonic in the input.
    std::vector<WATInstruction> operands; // Preparatory instructions of a folded expression, in source order.
};

struct WATFunctionResult {
    std::vector<ValueType> types;
    std::size_t end_offset{}; // Offset of the function's closing parenthesis.
};

struct WATDocument {
    std::vector<WATInstruction> instructions;
    // Absent for raw sequences; present (possibly empty types) for a wrapped function.
    std::optional<WATFunctionResult> function_result;
};

} // namespace limestone::wasmutils
