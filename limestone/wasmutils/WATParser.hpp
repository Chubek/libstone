#pragma once

#include "WAT-AST.hpp"

#include <string_view>

namespace limestone::wasmutils {

// Parse a raw (possibly folded) sequence or a single anonymous, zero-argument
// function in a module wrapper. Syntax errors throw Error at a byte offset.
// Folded instruction and block comment nesting are limited to 128 levels.
// Operand stack validation is performed separately by compile_wat.
WATDocument parse_wat(std::string_view input);

} // namespace limestone::wasmutils
