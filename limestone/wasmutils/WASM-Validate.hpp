#pragma once

#include "WASM-Module.hpp"

namespace limestone::wasmutils::core {

// Check Core 2.0 module and instruction validity. Throws wasmutils::Error
// on failure. Binary framing and section ordering are checked by decode_module.
void validate_module(const Module& module);

} // namespace limestone::wasmutils::core
