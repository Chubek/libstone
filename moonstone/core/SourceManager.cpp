#include "SourceManager.hpp"
#include <limits>
#include <mutex>
#include <shared_mutex>

namespace moonstone::inline v1 {
namespace {
Error failure(ErrorCategory category, std::string message, std::string operation) {
    return {category, std::move(message), {{std::move(operation), std::nullopt}}};
}

bool continuation(unsigned char c) { return (c & 0xc0) == 0x80; }

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t width;
        std::uint32_t code;
        if (c >= 0xc2 && c <= 0xdf) {
            width = 2;
            code = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            width = 3;
            code = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            width = 4;
            code = c & 0x07;
        } else
            return false;
        if (width > text.size() - i)
            return false;
        for (std::size_t j = 1; j < width; ++j) {
            auto next = static_cast<unsigned char>(text[i + j]);
            if (!continuation(next))
                return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((width == 3 && code < 0x800) || (width == 4 && code < 0x10000) ||
            (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff)
            return false;
        i += width;
    }
    return true;
}
} // namespace

struct SourceManager::Impl {
    SourceLimits limits;
    std::size_t bytes = 0;
    mutable std::shared_mutex mutex;
    std::vector<std::shared_ptr<const SourceSnapshot>> sources;
    explicit Impl(SourceLimits limits_) : limits(limits_) {}
};

SourceManager::SourceManager(SourceLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
SourceManager::~SourceManager() = default;

Result<SourceId> SourceManager::add(std::string file, std::string text) {
    std::unique_lock lock(impl_->mutex);
    auto remaining = impl_->limits.max_bytes - impl_->bytes;
    if (impl_->sources.size() >= impl_->limits.max_sources || file.size() > remaining ||
        text.size() > remaining - file.size() ||
        impl_->sources.size() >= std::numeric_limits<SourceId>::max()) {
        return failure(ErrorCategory::BudgetExhausted, "source storage limit", "add source");
    }
    if (!valid_utf8(text) || !valid_utf8(file) || file.find('\0') != std::string::npos) {
        return failure(ErrorCategory::InvalidInput, "invalid UTF-8 or NUL in filename",
                       "add source");
    }
    // SourceSpan uses uint32_t coordinates; reject rather than truncate.
    std::vector<std::size_t> lines{0};
    std::size_t column = 1;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            if (lines.size() == std::numeric_limits<std::uint32_t>::max()) {
                return failure(ErrorCategory::Unsupported,
                               "source line count exceeds v1 coordinates", "add source");
            }
            lines.push_back(i + 1);
            column = 1;
        } else {
            if (column == std::numeric_limits<std::uint32_t>::max()) {
                return failure(ErrorCategory::Unsupported, "source column exceeds v1 coordinates",
                               "add source");
            }
            ++column;
        }
    }
    auto id = static_cast<SourceId>(impl_->sources.size()) + 1;
    auto bytes = file.size() + text.size();
    auto source = std::make_shared<const SourceSnapshot>(
        SourceSnapshot{id, std::move(file), std::move(text), std::move(lines)});
    impl_->sources.push_back(std::move(source));
    impl_->bytes += bytes;
    return id;
}

Result<std::shared_ptr<const SourceSnapshot>> SourceManager::snapshot(SourceId id) const {
    std::shared_lock lock(impl_->mutex);
    if (id == invalid_source || id > impl_->sources.size()) {
        return failure(ErrorCategory::NotFound, "unknown source ID", "source snapshot");
    }
    return impl_->sources[static_cast<std::size_t>(id - 1)];
}

Result<SourceSpan> SourceManager::span(SourceId id, std::size_t begin, std::size_t end) const {
    auto found = snapshot(id);
    if (!found)
        return found.error();
    const auto &source = *found.value();
    auto boundary = [&](std::size_t offset) {
        return offset == source.text.size() ||
               !continuation(static_cast<unsigned char>(source.text[offset]));
    };
    if (begin > end || end > source.text.size() || !boundary(begin) || !boundary(end)) {
        return failure(ErrorCategory::InvalidInput, "invalid byte range or split UTF-8 scalar",
                       "source span");
    }
    auto position = [&](std::size_t offset) {
        auto line = std::upper_bound(source.line_starts.begin(), source.line_starts.end(), offset);
        auto index = static_cast<std::size_t>(line - source.line_starts.begin() - 1);
        return std::pair{static_cast<std::uint32_t>(index + 1),
                         static_cast<std::uint32_t>(offset - source.line_starts[index] + 1)};
    };
    auto [line, column] = position(begin);
    auto [end_line, end_column] = position(end);
    return SourceSpan{source.file, line, column, end_line, end_column};
}

struct SymbolTable::Impl {
    std::size_t max_symbols, max_bytes, bytes = 0;
    mutable std::shared_mutex mutex;
    // One owning copy of each spelling; reverse pointers remain valid in std::map.
    std::map<std::string, SymbolId, std::less<>> symbols;
    std::vector<const std::string *> names;
    Impl(std::size_t count, std::size_t limit) : max_symbols(count), max_bytes(limit) {}
};

SymbolTable::SymbolTable(std::size_t max_symbols, std::size_t max_bytes)
    : impl_(std::make_unique<Impl>(max_symbols, max_bytes)) {}
SymbolTable::~SymbolTable() = default;

Result<SymbolId> SymbolTable::intern(std::string symbol) {
    std::unique_lock lock(impl_->mutex);
    if (auto found = impl_->symbols.find(symbol); found != impl_->symbols.end())
        return found->second;
    if (symbol.empty())
        return failure(ErrorCategory::InvalidInput, "empty symbol", "intern symbol");
    if (impl_->names.size() >= impl_->max_symbols ||
        symbol.size() > impl_->max_bytes - impl_->bytes ||
        impl_->names.size() >= std::numeric_limits<SymbolId>::max()) {
        return failure(ErrorCategory::BudgetExhausted, "symbol storage limit", "intern symbol");
    }
    auto id = static_cast<SymbolId>(impl_->names.size()) + 1;
    auto [it, inserted] = impl_->symbols.emplace(std::move(symbol), id);
    (void)inserted;
    try {
        impl_->names.push_back(&it->first);
    } catch (...) {
        impl_->symbols.erase(it);
        throw;
    }
    impl_->bytes += it->first.size();
    return id;
}

Result<std::string> SymbolTable::lookup(SymbolId id) const {
    std::shared_lock lock(impl_->mutex);
    if (id == invalid_symbol || id > impl_->names.size()) {
        return failure(ErrorCategory::NotFound, "unknown symbol ID", "lookup symbol");
    }
    return *impl_->names[static_cast<std::size_t>(id - 1)];
}

} // namespace moonstone::inline v1
