#pragma once
#include "Result.hpp"
#include <memory>

namespace moonstone::inline v1 {

using SourceId = std::uint64_t;
inline constexpr SourceId invalid_source = 0;

struct SourceLimits {
    std::size_t max_sources = 4096;
    std::size_t max_bytes = 64 * 1024 * 1024; // Includes names and source contents.
};

struct SourceSnapshot {
    SourceId id;
    std::string file;
    std::string text;
    std::vector<std::size_t> line_starts;
};

class SourceManager {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    MOONSTONE_API explicit SourceManager(SourceLimits limits = {});
    MOONSTONE_API ~SourceManager();
    SourceManager(const SourceManager &) = delete;
    SourceManager &operator=(const SourceManager &) = delete;
    MOONSTONE_API Result<SourceId> add(std::string file, std::string text);
    MOONSTONE_API Result<std::shared_ptr<const SourceSnapshot>> snapshot(SourceId id) const;
    // Half-open UTF-8 byte range. Lines/byte columns are one-based, including EOF.
    MOONSTONE_API Result<SourceSpan> span(SourceId id, std::size_t begin, std::size_t end) const;
};

using SymbolId = std::uint64_t;
inline constexpr SymbolId invalid_symbol = 0;

class SymbolTable {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    MOONSTONE_API explicit SymbolTable(std::size_t max_symbols = 65536,
                                       std::size_t max_bytes = 4 * 1024 * 1024);
    MOONSTONE_API ~SymbolTable();
    SymbolTable(const SymbolTable &) = delete;
    SymbolTable &operator=(const SymbolTable &) = delete;
    MOONSTONE_API Result<SymbolId> intern(std::string symbol);
    MOONSTONE_API Result<std::string> lookup(SymbolId id) const;
};

} // namespace moonstone::inline v1
