#pragma once

#include <optional>

namespace novostone::tools {

// Call inside the CLI's exception boundary. nullopt means not a package command.
std::optional<int> pcc_package_command(int argc, char** argv);

} // namespace novostone::tools
