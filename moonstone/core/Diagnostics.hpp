#pragma once
#include "Core.hpp"

namespace moonstone::inline v1 {

struct DiagnosticOptions {
    std::size_t max_count = 1000;
    std::size_t max_bytes = 1024 * 1024;
    Severity minimum_severity = Severity::Note;
    std::set<std::string> suppressed_ids;
    std::size_t max_auxiliary_entries = 1024;
};

// Single-writer collector. Use one per analysis or externally synchronize.
class DiagnosticBuffer {
    DiagnosticOptions options_;
    Diagnostics diagnostics_;
    std::size_t bytes_ = 0, suppressed_ = 0, dropped_ = 0;
    bool has_errors_ = false;

  public:
    explicit DiagnosticBuffer(DiagnosticOptions options = {}) : options_(std::move(options)) {}
    MOONSTONE_API bool emit(Diagnostic diagnostic);
    const Diagnostics &diagnostics() const noexcept { return diagnostics_; }
    std::size_t retained_bytes() const noexcept { return bytes_; }
    std::size_t suppressed() const noexcept { return suppressed_; }
    std::size_t dropped() const noexcept { return dropped_; }
    bool exhausted() const noexcept { return dropped_ != 0; }
    bool has_errors() const noexcept { return has_errors_; }
};

} // namespace moonstone::inline v1
