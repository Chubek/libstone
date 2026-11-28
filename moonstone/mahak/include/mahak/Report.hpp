#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Model.hpp"
#include "Statistics.hpp"
#include "Storage.hpp"

namespace moonstone::mahak {
inline namespace v1 {

struct ReportOptions {
    bool include_raw = true;
    bool include_estimates = true;
    bool include_environment = true;
    bool color = false;
    bool markdown_tables = true;
    std::uint32_t width = 100;
    double confidence = 0.95;
    std::size_t max_rows = 200;
};

// Terminal table with explicit units, sample counts and validity markers.
std::string render_terminal(const SessionRecord &session, const ReportOptions &options = {});
// Markdown summary with estimates, distributions and environment differences.
std::string render_markdown(const SessionRecord &session, const ReportOptions &options = {});
// Standalone HTML with inline CSS; no external assets or scripts.
std::string render_html(const SessionRecord &session, const ReportOptions &options = {});
// Lossless CSV export (same rows as Storage::session_csv, exposed here for
// report-only workflows).
std::string render_csv(const SessionRecord &session);

struct ComparisonBundle {
    SessionRecord baseline;
    SessionRecord candidate;
    std::vector<Comparison> comparisons;
    std::vector<std::string> notes;
};

// Aligns runs by (benchmark, target, configuration) and compares the primary
// metrics. Missing counterpart runs are reported, never silently paired.
Result<ComparisonBundle> compare_sessions(const SessionRecord &baseline,
                                          const SessionRecord &candidate,
                                          const ComparisonOptions &options = {});

std::string render_comparison_terminal(const ComparisonBundle &bundle,
                                       const ReportOptions &options = {});
std::string render_comparison_markdown(const ComparisonBundle &bundle,
                                       const ReportOptions &options = {});

// Historical trend summary across committed sessions for one metric. Sessions
// are ordered by created_unix_ns; incomplete sessions are labelled, not hidden.
std::string render_history(const std::vector<SessionRecord> &sessions,
                           std::string_view metric,
                           const ReportOptions &options = {});

} // namespace v1
} // namespace moonstone::mahak
