#ifndef CEPHYR_CACHE_HPP
#define CEPHYR_CACHE_HPP

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cephyr {

class Diagnostics;

using GeneratedFiles = std::map<std::string, std::string>;

// An optional, multiprocess-safe cache for generated source trees.
//
// Each fingerprint (1..511 bytes) identifies one checksummed record. Writers
// stage and atomically replace complete generated trees; concurrent readers
// see the old or new snapshot. An empty directory disables caching silently.
// Unsafe paths, corrupt records and storage failures warn and behave as misses.
// New files and directories are private. The cache has no eviction policy.
class Cache {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    Cache(const std::filesystem::path &, Diagnostics &);
    ~Cache();

    Cache(const Cache &) = delete;
    Cache &operator=(const Cache &) = delete;

    std::optional<GeneratedFiles> get(std::string_view key);
    void put(std::string_view key, const GeneratedFiles &);
};

} // namespace cephyr

#endif
