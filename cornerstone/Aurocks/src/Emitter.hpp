#pragma once

#include "aurocks/Compiler.hpp"

namespace aurocks::detail {

// Validate action references, policy directives and portable forest
// semantics without constructing generated C output.
void validateEmission(const Grammar&, const Analysis&, std::string_view entrypoint,
                      std::vector<Diagnostic>&);

} // namespace aurocks::detail
