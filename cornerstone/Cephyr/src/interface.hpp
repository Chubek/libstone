#pragma once

#include <optional>
#include <string>

namespace cparser { class ASTNode; }

namespace cephyr {

class Diagnostics;
struct Preprocessed;
struct Target;

// The caller supplies a parsed translation unit and the usual semantic target properties.
std::optional<std::string> project_interface(cparser::ASTNode *, const Preprocessed &,
                                             Diagnostics &, bool hosted,
                                             const Target &, bool factir, bool moonstone);

} // namespace cephyr
