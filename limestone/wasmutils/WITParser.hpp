#pragma once

#include "WIT-IDL.hpp"

namespace limestone::wasmutils {

// Parses one WIT 0.2 source file into an owning, validated AST. Qualified
// references to other packages are retained symbolically; no dependencies
// are loaded. Throws WitError for syntax errors or invalid local references.
WitDocument parse_wit(std::string_view source);

// Checks AST structure and local references, including manually built ASTs.
// Neither function resolves dependencies in other files or packages.
void validate_wit(const WitDocument& document);

} // namespace limestone::wasmutils
