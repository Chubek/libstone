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

namespace core {

struct Module;

// Validate and lower a Core 2.0 module to a standalone ANSI C90 translation
// unit. The emitted declarations describe the instance/host-import API,
// typed status codes, and legacy wasm_module_invoke compatibility wrapper.
// Supports scalar numerics, references, function imports, tables, globals,
// memory, data/element segments, and start. Non-function imports and SIMD
// are diagnosed. Floating-point arithmetic needs an IEEE binary32/binary64
// C host (round-to-nearest-even, no fast-math) and its C math library.
std::string emit_module_c(const Module& module);

} // namespace core

} // namespace limestone::wasmutils
