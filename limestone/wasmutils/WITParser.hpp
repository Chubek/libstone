#pragma once

#include "WIT-IDL.hpp"

namespace limestone::wasmutils {

// Parses the standalone subset in WIT.g into an owning, validated AST.
// Optional package (with numeric major.minor.patch version), one or more
// interfaces, synchronous scalar-only functions with zero or one result.
// ASCII source; whitespace, line comments and nested block comments are
// ignored. No worlds, annotations, imports, type definitions, or aggregates.
// Syntax, unsupported features and duplicate names throw WitError.
WitDocument parse_wit(std::string_view source);

} // namespace limestone::wasmutils
