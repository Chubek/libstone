#pragma once

#include "WASM-Module.hpp"

namespace limestone::wasmutils::core {

// Assemble a Core 2.0 WAT module or a sequence of module fields (not a WAST
// script). Errors report a source byte offset. Syntax, comment, and structured
// control nesting are bounded; stack types are checked by validate_module.
Module assemble_module(std::string_view input);

} // namespace limestone::wasmutils::core
