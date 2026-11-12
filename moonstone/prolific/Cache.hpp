#pragma once
#include "Prolific.hpp"

#include <filesystem>
#include <optional>

namespace moonstone::prolific {

// One live Cache per canonical path per process; methods on that object are
// serialized and LMDB coordinates other processes. Blocking, not signal-safe.
// Writers create directories; read-only opens never create them. Exceptions
// retain LMDB/OS causes. A failed transaction never replaces an older run.
class Cache {
  public:
    explicit Cache(const std::filesystem::path &directory, bool read_only = false,
                   std::size_t map_bytes = 256 * 1024 * 1024);
    ~Cache();
    Cache(const Cache &) = delete;
    Cache &operator=(const Cache &) = delete;
    void save(std::string_view run, const Report &report, bool replace = false);
    std::optional<Report> load(std::string_view run) const;
    std::optional<std::string> json(std::string_view run) const;
    std::vector<std::string> runs() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moonstone::prolific
