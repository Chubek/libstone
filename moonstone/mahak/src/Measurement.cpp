#include "mahak/Measurement.hpp"

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <time.h>
#include <vector>

#include "mahak/Json.hpp"

namespace moonstone::mahak {
inline namespace v1 {
namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

void parse_stream(std::string_view text, std::string_view source, MetricParseResult &result) {
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::size_t stop = end == std::string_view::npos ? text.size() : end;
        const std::string_view line = trim(text.substr(begin, stop - begin));
        begin = stop + 1;
        if (line.empty())
            continue;
        bool is_metric = starts_with(line, "MAHAK-METRIC");
        bool is_phase = starts_with(line, "MAHAK-PHASE");
        if (!is_metric && !is_phase)
            continue;
        std::string_view payload = is_metric ? line.substr(12) : line.substr(11);
        payload = trim(payload);
        auto parsed = Json::parse(payload, source);
        if (!parsed || !parsed.value().is_object()) {
            result.warnings.push_back("malformed " +
                                      std::string(is_metric ? "MAHAK-METRIC" : "MAHAK-PHASE") +
                                      " record in " + std::string(source));
            continue;
        }
        const Json *name = parsed.value().find("name");
        const std::string *name_text = name != nullptr ? name->as_string() : nullptr;
        if (name_text == nullptr || name_text->empty()) {
            result.warnings.push_back("metric record without a name in " + std::string(source));
            continue;
        }
        if (is_metric) {
            const Json *unit = parsed.value().find("unit");
            const std::string *unit_text = unit != nullptr ? unit->as_string() : nullptr;
            const Json *value = parsed.value().find("value");
            const auto number = value != nullptr ? value->as_double() : std::nullopt;
            if (unit_text == nullptr || !number) {
                result.warnings.push_back("metric record without unit/value in " +
                                          std::string(source));
                continue;
            }
            RuntimeMetric metric;
            metric.name = *name_text;
            metric.unit = *unit_text;
            metric.value = *number;
            const Json *from = parsed.value().find("source");
            const std::string *source_text = from != nullptr ? from->as_string() : nullptr;
            metric.source = source_text != nullptr ? *source_text
                                                   : "runtime[" + std::string(source) + "]";
            result.runtime.push_back(std::move(metric));
        } else {
            PhaseEvent phase;
            phase.name = *name_text;
            const Json *category = parsed.value().find("category");
            const std::string *category_text =
                category != nullptr ? category->as_string() : nullptr;
            phase.category = category_text != nullptr ? *category_text : "custom";
            const Json *start = parsed.value().find("start_ns");
            const auto start_value = start != nullptr ? start->as_uint64() : std::nullopt;
            const Json *duration = parsed.value().find("duration_ns");
            const auto duration_value = duration != nullptr ? duration->as_uint64() : std::nullopt;
            if (!start_value || !duration_value) {
                result.warnings.push_back("phase record without timing in " +
                                          std::string(source));
                continue;
            }
            phase.start_ns = *start_value;
            phase.duration_ns = *duration_value;
            const Json *detail = parsed.value().find("detail");
            const std::string *detail_text = detail != nullptr ? detail->as_string() : nullptr;
            phase.detail = detail_text != nullptr ? *detail_text : std::string();
            result.phases.push_back(std::move(phase));
        }
    }
}

} // namespace

std::uint64_t monotonic_now_ns() noexcept {
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(value.tv_nsec);
}

std::uint64_t unix_now_ns() noexcept {
    timespec value{};
    if (::clock_gettime(CLOCK_REALTIME, &value) != 0)
        return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(value.tv_nsec);
}

MetricParseResult parse_metric_records(std::string_view text, std::string_view source) {
    MetricParseResult result;
    parse_stream(text, source, result);
    return result;
}

MetricParseResult collect_command_metrics(const CommandResult &result) {
    MetricParseResult combined;
    auto merge = [&combined](MetricParseResult parsed) {
        combined.phases.insert(combined.phases.end(), parsed.phases.begin(), parsed.phases.end());
        combined.runtime.insert(combined.runtime.end(), parsed.runtime.begin(),
                                parsed.runtime.end());
        combined.warnings.insert(combined.warnings.end(), parsed.warnings.begin(),
                                 parsed.warnings.end());
    };
    merge(parse_metric_records(result.stdout_text, "stdout"));
    merge(parse_metric_records(result.stderr_text, "stderr"));
    return combined;
}

} // namespace v1
} // namespace moonstone::mahak
