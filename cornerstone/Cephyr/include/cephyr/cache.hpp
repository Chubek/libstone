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
// `key` is the complete generation fingerprint supplied by the driver. Cache
// never truncates or otherwise rewrites it: a key too large for LMDB bypasses
// caching instead. Values contain only generated text files, not compiler
// outputs. A hit still has to be published through Archon's
// write_generated_directory API so replacement, input protection, and
// destination symlink checks are identical to an uncached generation.
//
// Cache failures are warnings and behave as misses. One Cache instance should
// be kept for each cache directory in a process; LMDB supports independent
// reader/writer processes, but does not support opening one environment twice
// in a single process. An empty directory intentionally disables caching
// without a warning. Newly created cache directories are private; existing
// directories and LMDB files must be user-owned and not group/world-writable.
// The cache directory itself and LMDB files must not be symlinks.
// The fixed LMDB map reserves virtual address space
// (16 GiB on 64-bit hosts, 1 GiB on 32-bit hosts), has no eviction policy, and
// falls back when full. The record checksum detects accidental damage; it is
// not authentication for a cache directory controlled by an attacker.
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
