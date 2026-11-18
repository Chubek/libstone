#pragma once

#include "WAT-AST.hpp"

#include <string_view>

namespace limestone::wasmutils {

// Lower in stack order, validate the program and check any declared results.
// Syntax and validation errors throw Error at a byte offset in the WAT input.
Program compile_wat(std::string_view input);
Program compile_wat_ast(WATDocument const& document);

} // namespace limestone::wasmutils
