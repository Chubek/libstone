#pragma once

#include "aurocks/Compiler.hpp"

#include <iosfwd>

namespace aurocks::detail {

// Append the LibGLR tables and public entry points after aurocks_action.
void emitLinkedParser(std::ostream& out, const Grammar& grammar, const Analysis& analysis,
                      std::string_view entrypoint, std::vector<Diagnostic>& diagnostics);

} // namespace aurocks::detail
