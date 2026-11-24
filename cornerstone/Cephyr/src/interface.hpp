#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cparser { class ASTNode; }

namespace cephyr {

class Diagnostics;
struct Preprocessed;

// The caller supplies a parsed translation unit and the usual semantic target properties.
std::optional<std::string> project_interface(cparser::ASTNode *, const Preprocessed &,
                                             Diagnostics &, bool hosted,
                                             std::string_view triple, std::string_view abi);

} // namespace cephyr
