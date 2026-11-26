#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace asdl::detail {

inline std::filesystem::path executable_path(const std::filesystem::path& invocation,
                                             const char* failure)
{
    namespace fs = std::filesystem;
    std::error_code ec;
#if defined(__linux__)
    auto proc_path = fs::canonical("/proc/self/exe", ec);
    if (!ec) return proc_path;
#endif
    if (invocation.has_parent_path()) {
        auto path = fs::canonical(invocation, ec);
        if (!ec) return path;
    } else if (const char* path_var = std::getenv("PATH")) {
        std::string_view path(path_var);
        while (true) {
            const auto next = path.find(':');
            const auto entry = fs::path(path.substr(0, next)) / invocation;
            auto candidate = fs::canonical(entry, ec);
            if (!ec && fs::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0)
                return candidate;
            if (next == path.npos) break;
            path.remove_prefix(next + 1);
        }
    }
    throw std::runtime_error(failure);
}

} // namespace asdl::detail
