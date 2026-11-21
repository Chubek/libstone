#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cephyr {

class Diagnostics;

// Check argv[0] without spawning a tool; absolute/relative paths and PATH
// lookup follow the same convention as run_process.
bool executable_available(std::string_view command);

// Execute argv directly, with no shell expansion. Capture the tool's bounded
// output for the diagnostic collector. Explicit descriptors (all > 2) are
// inherited by this tool only; their CLOEXEC flags in the parent are unchanged.
// When stdout_fd is supplied, only stderr is captured for diagnostics.
int run_process(const std::vector<std::string> &argv, Diagnostics &diagnostics,
                const std::filesystem::path &cwd = {}, std::span<const int> descriptors = {},
                int stdout_fd = -1);

} // namespace cephyr
