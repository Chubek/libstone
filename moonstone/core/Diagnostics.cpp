#include "Diagnostics.hpp"
#include <limits>

namespace moonstone::inline v1 {
namespace {
void increment(std::size_t &count) {
    if (count != std::numeric_limits<std::size_t>::max())
        ++count;
}
} // namespace

bool DiagnosticBuffer::emit(Diagnostic diagnostic) {
    // Unknown severity values must not make a host's error gate fail open.
    if (diagnostic.severity != Severity::Note && diagnostic.severity != Severity::Warning)
        has_errors_ = true;
    if (diagnostic.severity < options_.minimum_severity ||
        options_.suppressed_ids.count(diagnostic.id)) {
        increment(suppressed_);
        return false;
    }
    auto remaining = options_.max_bytes - bytes_;
    auto take = [&](std::string_view value) {
        if (value.size() > remaining)
            return false;
        remaining -= value.size();
        return true;
    };
    auto entries = options_.max_auxiliary_entries;
    auto take_entries = [&](std::size_t count) {
        if (count > entries)
            return false;
        entries -= count;
        return true;
    };
    bool fits = diagnostics_.size() < options_.max_count &&
                take_entries(diagnostic.related.size()) && take_entries(diagnostic.notes.size()) &&
                take_entries(diagnostic.fixes.size()) && take(diagnostic.id) &&
                take(diagnostic.message) && take(diagnostic.primary.file);
    for (const auto &span : diagnostic.related)
        fits = fits && take(span.file);
    for (const auto &note : diagnostic.notes)
        fits = fits && take(note);
    for (const auto &fix : diagnostic.fixes) {
        fits = fits && take(fix.span.file) && take(fix.replacement) && take(fix.description);
    }
    if (!fits) {
        increment(dropped_);
        return false;
    }
    diagnostics_.push_back(std::move(diagnostic));
    bytes_ = options_.max_bytes - remaining;
    return true;
}

} // namespace moonstone::inline v1
