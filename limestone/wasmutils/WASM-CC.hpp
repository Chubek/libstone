#pragma once

#include "WASM-IR.hpp"

#include <span>
#include <string>

namespace limestone::wasmutils {

// Validate a WASM.aml opcode stream and emit a self-contained ANSI C89/C90
// translation unit. Invalid streams throw Error with the instruction offset.
//
// Generated ABI (size_t from <stddef.h>):
//   struct wasm_result { unsigned width; unsigned long lo, hi; };
//   int wasm_run(struct wasm_result *out, size_t capacity, size_t *count);
//
// Success (0) writes results in stack order, bottom first. Width is 32 or 64;
// lo and hi hold 32-bit limbs, with hi == 0 on i32. Capacity is in elements.
// Failure (1: unreachable trap, 2: insufficient capacity or missing buffer)
// leaves the result buffer unchanged. Optional count receives the required
// result count (zero on trap) and must not alias the result buffer. When there
// are no results, out may be null and capacity is ignored.
std::string emit_c(std::span<const Instruction> program);

} // namespace limestone::wasmutils
