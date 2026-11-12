/** @file TemporaryDirectory.hpp @brief Private per-operation files with RAII cleanup. */
#ifndef JITERATI_INTERNAL_TEMPORARY_DIRECTORY_HPP
#define JITERATI_INTERNAL_TEMPORARY_DIRECTORY_HPP
#include <filesystem>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#include <random>
#else
#include <cstdlib>
#include <vector>
#endif

namespace jiterati::detail {
class TemporaryDirectory {
public:
    TemporaryDirectory() {
#if defined(_WIN32)
        std::random_device random;
        for (unsigned i = 0; i < 64; ++i) {
            auto path = std::filesystem::temp_directory_path() / ("jiterati-" + std::to_string(random()) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(path)) { path_ = std::move(path); return; }
        }
        throw std::runtime_error("native: cannot create private temporary directory");
#else
        auto pattern = (std::filesystem::temp_directory_path() / "jiterati-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end()); writable.push_back(0);
        auto* path = ::mkdtemp(writable.data());
        if (!path) throw std::runtime_error("native: cannot create private temporary directory");
        path_ = path;
#endif
    }
    TemporaryDirectory(TemporaryDirectory const&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory const&) = delete;
    ~TemporaryDirectory() { std::error_code ec; std::filesystem::remove_all(path_, ec); }
    std::string file(char const* name) const { return (path_ / name).string(); }
private:
    std::filesystem::path path_;
};
} // namespace jiterati::detail
#endif
