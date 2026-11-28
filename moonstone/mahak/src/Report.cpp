#include "mahak/Report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moonstone::mahak {
inline namespace v1 {
namespace {

struct MetricSeries {
    std::vector<Observation> observations;
    std::string unit;
    MetricDirection direction = MetricDirection::LowerIsBetter;
};

std::map<std::string, MetricSeries> series_by_metric(const RunRecord &run) {
    std::map<std::string, MetricSeries> series;
    for (const auto &sample : run.samples) {
        const Observation &observation = sample.observation;
        if (observation.metric.empty())
            continue;
        MetricSeries &target = series[observation.metric];
        target.observations.push_back(observation);
        if (target.unit.empty())
            target.unit = observation.unit;
        target.direction = observation.direction;
    }
    return series;
}

std::string configuration_text(const RunRecord &run) {
    if (!run.configuration.empty())
        return run.configuration;
    std::string text;
    for (const auto &parameter : run.parameters) {
        if (!text.empty())
            text += ",";
        text += parameter.name + "=" + parameter.text;
    }
    return text.empty() ? std::string("-") : text;
}

std::string format_number(double value) {
    if (!std::isfinite(value))
        return "n/a";
    char buffer[64];
    if (std::fabs(value) >= 1000.0 || value == 0.0)
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    else if (std::fabs(value) >= 1.0)
        std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    else
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char character : text) {
        switch (character) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(character);
        }
    }
    return out;
}

std::string markdown_escape(std::string_view text) {
    std::string out;
    for (char character : text) {
        if (character == '|')
            out += "\\|";
        else
            out.push_back(character);
    }
    return out;
}

std::string pad(std::string_view text, std::size_t width) {
    if (text.size() >= width)
        return std::string(text);
    return std::string(text) + std::string(width - text.size(), ' ');
}

std::string truncate(std::string_view text, std::size_t width) {
    if (text.size() <= width)
        return std::string(text);
    if (width <= 3)
        return std::string(text.substr(0, width));
    return std::string(text.substr(0, width - 3)) + "...";
}

std::string environment_differences(const EnvironmentInfo &baseline,
                                    const EnvironmentInfo &candidate) {
    std::string out;
    auto note = [&out](std::string_view key, const std::string &left, const std::string &right) {
        if (left == right)
            return;
        out += "  ";
        out += key;
        out += ": '";
        out += left;
        out += "' vs '";
        out += right;
        out += "'\n";
    };
    note("os", baseline.os, candidate.os);
    note("kernel", baseline.kernel, candidate.kernel);
    note("architecture", baseline.architecture, candidate.architecture);
    note("cpu_model", baseline.cpu_model, candidate.cpu_model);
    note("compiler_version", baseline.compiler_version, candidate.compiler_version);
    return out;
}

} // namespace

std::string render_terminal(const SessionRecord &session, const ReportOptions &options) {
    std::ostringstream out;
    out << "session  " << session.id << "  [" << session.status << "]\n";
    out << "manifest " << session.manifest_id << " " << session.manifest_version
        << "  digest " << session.manifest_digest << "\n";
    if (options.include_environment) {
        out << "host     " << session.host << "\n";
        out << "os       " << session.environment.os << " " << session.environment.architecture
            << "  cpus=" << session.environment.logical_cpus << "\n";
    }
    out << "\n";
    std::size_t rows = 0;
    for (const auto &run : session.runs) {
        if (rows++ >= options.max_rows) {
            out << "... (" << session.runs.size() - options.max_rows << " more runs)\n";
            break;
        }
        out << run.benchmark_id << " @ " << run.target_id << "  [" << run.status;
        if (!run.oracle.passed)
            out << ", oracle=" << name(run.oracle.validity);
        out << "]  " << truncate(configuration_text(run), 60) << "\n";
        if (!run.message.empty())
            out << "  message: " << run.message << "\n";
        if (!run.oracle.passed && !run.oracle.message.empty())
            out << "  oracle: " << run.oracle.message << "\n";
        const auto series = series_by_metric(run);
        if (series.empty()) {
            out << "  no samples\n";
            continue;
        }
        out << "  " << pad("metric", 18) << pad("unit", 8) << pad("n", 6) << pad("valid", 7)
            << pad("median", 12) << pad("mean", 12) << pad("rsd", 9) << "ci95\n";
        for (const auto &entry : series) {
            const auto &observations = entry.second.observations;
            const Estimates estimates = estimate(observations);
            std::vector<double> values;
            for (const auto &observation : observations) {
                if (observation.valid && std::isfinite(observation.value))
                    values.push_back(observation.value);
            }
            BootstrapOptions bootstrap;
            bootstrap.seed = 0x5eed;
            bootstrap.confidence = options.confidence;
            const ConfidenceInterval interval = bootstrap_confidence_interval(values, bootstrap);
            std::string interval_text = "n/a";
            if (interval.defined)
                interval_text = "[" + format_number(interval.lower) + ", " +
                                format_number(interval.upper) + "]";
            out << "  " << pad(truncate(entry.first, 18), 18) << pad(entry.second.unit, 8)
                << pad(std::to_string(estimates.total), 6)
                << pad(std::to_string(estimates.valid), 7) << pad(format_number(estimates.median), 12)
                << pad(format_number(estimates.mean), 12)
                << pad(format_number(estimates.relative_standard_deviation), 9) << interval_text
                << "\n";
        }
        if (options.include_raw) {
            out << "  raw:";
            for (const auto &entry : series) {
                out << " " << entry.first << "=[";
                bool first = true;
                for (const auto &observation : entry.second.observations) {
                    if (!first)
                        out << " ";
                    first = false;
                    out << format_number(observation.value);
                    if (!observation.valid)
                        out << "!";
                }
                out << "]";
            }
            out << "\n";
        }
    }
    if (!session.notes.empty()) {
        out << "\nnotes:\n";
        for (const auto &note : session.notes)
            out << "  - " << note << "\n";
    }
    return out.str();
}

std::string render_markdown(const SessionRecord &session, const ReportOptions &options) {
    std::ostringstream out;
    out << "# Mahak session `" << session.id << "`\n\n";
    out << "- status: `" << session.status << "`\n";
    out << "- manifest: `" << session.manifest_id << "` " << session.manifest_version << "\n";
    out << "- digest: `" << session.manifest_digest << "`\n";
    if (options.include_environment)
        out << "- host: " << session.host << " (" << session.environment.os << ", "
            << session.environment.architecture << ")\n";
    out << "\n";
    for (const auto &run : session.runs) {
        out << "## " << run.benchmark_id << " @ " << run.target_id << "\n\n";
        out << "| case | status | configuration | oracle |\n|---|---|---|---|\n";
        out << "| " << markdown_escape(run.case_id) << " | " << run.status << " | "
            << markdown_escape(configuration_text(run)) << " | "
            << (run.oracle.passed ? "pass" : name(run.oracle.validity)) << " |\n\n";
        const auto series = series_by_metric(run);
        if (series.empty())
            continue;
        out << "| metric | unit | n | valid | median | mean | rsd | ci95 |\n";
        out << "|---|---|---|---|---|---|---|---|\n";
        for (const auto &entry : series) {
            const Estimates estimates = estimate(entry.second.observations);
            std::vector<double> values;
            for (const auto &observation : entry.second.observations) {
                if (observation.valid && std::isfinite(observation.value))
                    values.push_back(observation.value);
            }
            BootstrapOptions bootstrap;
            bootstrap.seed = 0x5eed;
            bootstrap.confidence = options.confidence;
            const ConfidenceInterval interval = bootstrap_confidence_interval(values, bootstrap);
            out << "| " << entry.first << " | " << entry.second.unit << " | " << estimates.total
                << " | " << estimates.valid << " | " << format_number(estimates.median) << " | "
                << format_number(estimates.mean) << " | "
                << format_number(estimates.relative_standard_deviation) << " | ";
            if (interval.defined)
                out << "[" << format_number(interval.lower) << ", "
                    << format_number(interval.upper) << "]";
            else
                out << "n/a";
            out << " |\n";
        }
        out << "\n";
    }
    if (!session.notes.empty()) {
        out << "## Notes\n\n";
        for (const auto &note : session.notes)
            out << "- " << markdown_escape(note) << "\n";
    }
    return out.str();
}

std::string render_html(const SessionRecord &session, const ReportOptions &options) {
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n";
    out << "<title>Mahak session " << html_escape(session.id) << "</title>\n";
    out << "<style>body{font-family:system-ui,sans-serif;margin:2rem;color:#111}"
           "table{border-collapse:collapse;margin:1rem 0}"
           "th,td{border:1px solid #ccc;padding:0.3rem 0.6rem;text-align:left}"
           "th{background:#f4f4f4}code{background:#f4f4f4;padding:0.1rem 0.3rem}"
           ".invalid{color:#a33}</style></head><body>\n";
    out << "<h1>Session <code>" << html_escape(session.id) << "</code></h1>\n";
    out << "<p>status <code>" << html_escape(session.status) << "</code>, manifest <code>"
        << html_escape(session.manifest_id) << "</code> " << html_escape(session.manifest_version)
        << ", digest <code>" << html_escape(session.manifest_digest) << "</code></p>\n";
    if (options.include_environment)
        out << "<p>host " << html_escape(session.host) << ", " << html_escape(session.environment.os)
            << " " << html_escape(session.environment.architecture) << "</p>\n";
    for (const auto &run : session.runs) {
        out << "<h2>" << html_escape(run.benchmark_id) << " @ " << html_escape(run.target_id)
            << "</h2>\n";
        out << "<p>status <code>" << html_escape(run.status) << "</code>, oracle "
            << (run.oracle.passed ? "pass" : html_escape(name(run.oracle.validity))) << "</p>\n";
        const auto series = series_by_metric(run);
        if (series.empty())
            continue;
        out << "<table><thead><tr><th>metric</th><th>unit</th><th>n</th><th>valid</th>"
               "<th>median</th><th>mean</th><th>rsd</th><th>ci95</th></tr></thead><tbody>\n";
        for (const auto &entry : series) {
            const Estimates estimates = estimate(entry.second.observations);
            std::vector<double> values;
            for (const auto &observation : entry.second.observations) {
                if (observation.valid && std::isfinite(observation.value))
                    values.push_back(observation.value);
            }
            BootstrapOptions bootstrap;
            bootstrap.seed = 0x5eed;
            bootstrap.confidence = options.confidence;
            const ConfidenceInterval interval = bootstrap_confidence_interval(values, bootstrap);
            out << "<tr><td>" << html_escape(entry.first) << "</td><td>"
                << html_escape(entry.second.unit) << "</td><td>" << estimates.total << "</td><td>"
                << estimates.valid << "</td><td>" << format_number(estimates.median) << "</td><td>"
                << format_number(estimates.mean) << "</td><td>"
                << format_number(estimates.relative_standard_deviation) << "</td><td>";
            if (interval.defined)
                out << "[" << format_number(interval.lower) << ", "
                    << format_number(interval.upper) << "]";
            else
                out << "n/a";
            out << "</td></tr>\n";
        }
        out << "</tbody></table>\n";
    }
    out << "</body></html>\n";
    return out.str();
}

std::string render_csv(const SessionRecord &session) {
    auto csv = session_csv(session);
    return csv ? csv.value() : std::string();
}

Result<ComparisonBundle> compare_sessions(const SessionRecord &baseline,
                                          const SessionRecord &candidate,
                                          const ComparisonOptions &options) {
    ComparisonBundle bundle;
    bundle.baseline = baseline;
    bundle.candidate = candidate;

    auto key_of = [](const RunRecord &run) {
        return run.benchmark_id + "\u0000" + run.target_id + "\u0000" + run.configuration;
    };
    std::map<std::string, const RunRecord *> baseline_runs;
    for (const auto &run : baseline.runs)
        baseline_runs.emplace(key_of(run), &run);
    std::map<std::string, const RunRecord *> candidate_runs;
    for (const auto &run : candidate.runs)
        candidate_runs.emplace(key_of(run), &run);

    const std::string environment = environment_differences(baseline.environment,
                                                            candidate.environment);
    if (!environment.empty())
        bundle.notes.push_back("environment differs:\n" + environment);

    for (const auto &entry : baseline_runs) {
        const auto found = candidate_runs.find(entry.first);
        if (found == candidate_runs.end()) {
            bundle.notes.push_back("no candidate run for " + entry.second->case_id);
            continue;
        }
        const RunRecord &left = *entry.second;
        const RunRecord &right = *found->second;
        const auto left_series = series_by_metric(left);
        const auto right_series = series_by_metric(right);
        std::map<std::string, bool> metrics;
        for (const auto &series : left_series)
            metrics[series.first] = true;
        for (const auto &series : right_series)
            metrics[series.first] = true;
        for (const auto &metric : metrics) {
            const std::vector<Observation> empty;
            const auto left_it = left_series.find(metric.first);
            const auto right_it = right_series.find(metric.first);
            const std::vector<Observation> &left_observations =
                left_it != left_series.end() ? left_it->second.observations : empty;
            const std::vector<Observation> &right_observations =
                right_it != right_series.end() ? right_it->second.observations : empty;
            if (left_observations.empty() || right_observations.empty()) {
                bundle.notes.push_back("metric '" + metric.first + "' missing on " +
                                       (left_observations.empty() ? "baseline" : "candidate") +
                                       " for " + left.case_id);
                continue;
            }
            auto compared = compare_observations(left_observations, right_observations, options);
            if (!compared)
                return compared.error();
            // ComparisonResult has no case field in the v1 contract; the run
            // identity is carried in the message so reports stay attributable.
            const std::string identity =
                left.case_id == right.case_id ? left.case_id
                                              : left.case_id + " vs " + right.case_id;
            if (compared.value().result.message.empty())
                compared.value().result.message = identity;
            else
                compared.value().result.message = identity + "; " + compared.value().result.message;
            bundle.comparisons.push_back(compared.value());
        }
    }
    for (const auto &entry : candidate_runs) {
        if (baseline_runs.find(entry.first) == baseline_runs.end())
            bundle.notes.push_back("no baseline run for " + entry.second->case_id);
    }
    return bundle;
}

std::string render_comparison_terminal(const ComparisonBundle &bundle,
                                       const ReportOptions &options) {
    std::ostringstream out;
    out << "baseline  " << bundle.baseline.id << "\n";
    out << "candidate " << bundle.candidate.id << "\n\n";
    out << pad("case", 30) << pad("metric", 18) << pad("unit", 8) << pad("baseline", 14)
        << pad("candidate", 14) << pad("ratio", 9) << pad("change%", 10) << "verdict\n";
    for (const auto &comparison : bundle.comparisons) {
        const ComparisonResult &result = comparison.result;
        out << pad(truncate(result.message, 30), 30) << pad(truncate(result.metric, 18), 18)
            << pad(result.unit, 8) << pad(format_number(result.baseline_mean), 14)
            << pad(format_number(result.candidate_mean), 14)
            << pad(format_number(result.ratio), 9) << pad(format_number(result.percent_change), 10)
            << result.verdict << "\n";
    }
    if (!bundle.notes.empty()) {
        out << "\nnotes:\n";
        for (const auto &note : bundle.notes)
            out << "  - " << note << "\n";
    }
    if (options.markdown_tables)
        out << "\n";
    return out.str();
}

std::string render_comparison_markdown(const ComparisonBundle &bundle,
                                       const ReportOptions &options) {
    (void)options;
    std::ostringstream out;
    out << "# Mahak comparison\n\n";
    out << "- baseline: `" << bundle.baseline.id << "`\n";
    out << "- candidate: `" << bundle.candidate.id << "`\n\n";
    out << "| metric | unit | baseline | candidate | ratio | change% | verdict |\n";
    out << "|---|---|---|---|---|---|---|\n";
    for (const auto &comparison : bundle.comparisons) {
        const ComparisonResult &result = comparison.result;
        out << "| " << markdown_escape(result.metric) << " | " << result.unit << " | "
            << format_number(result.baseline_mean) << " | "
            << format_number(result.candidate_mean) << " | " << format_number(result.ratio) << " | "
            << format_number(result.percent_change) << " | " << result.verdict << " |\n";
    }
    if (!bundle.notes.empty()) {
        out << "\n## Notes\n\n";
        for (const auto &note : bundle.notes)
            out << "- " << markdown_escape(note) << "\n";
    }
    return out.str();
}

std::string render_history(const std::vector<SessionRecord> &sessions, std::string_view metric,
                           const ReportOptions &options) {
    (void)options;
    std::vector<const SessionRecord *> ordered;
    ordered.reserve(sessions.size());
    for (const auto &session : sessions)
        ordered.push_back(&session);
    std::sort(ordered.begin(), ordered.end(),
              [](const SessionRecord *left, const SessionRecord *right) {
                  if (left->created_unix_ns != right->created_unix_ns)
                      return left->created_unix_ns < right->created_unix_ns;
                  return left->id < right->id;
              });
    std::ostringstream out;
    out << "history for metric '" << metric << "'\n";
    out << pad("session", 24) << pad("created_unix_ns", 18) << pad("status", 12)
        << pad("n", 6) << pad("valid", 7) << "median\n";
    for (const SessionRecord *session : ordered) {
        std::vector<Observation> observations;
        for (const auto &run : session->runs) {
            for (const auto &sample : run.samples) {
                if (sample.observation.metric == metric)
                    observations.push_back(sample.observation);
            }
        }
        const Estimates estimates = estimate(observations);
        out << pad(truncate(session->id, 24), 24)
            << pad(std::to_string(session->created_unix_ns), 18)
            << pad(session->status.empty() ? "unknown" : session->status, 12)
            << pad(std::to_string(estimates.total), 6)
            << pad(std::to_string(estimates.valid), 7)
            << (estimates.valid == 0 ? "n/a" : format_number(estimates.median)) << "\n";
    }
    return out.str();
}

} // namespace v1
} // namespace moonstone::mahak
