#include "mahak/Plugin.hpp"
#include "mahak/Profile.hpp"
#include "mahak/Recommend.hpp"
#include "mahak/Runner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace moonstone::mahak {
inline namespace v1 {
namespace {

Error contract_error(std::string message, ErrorCategory category = ErrorCategory::InvalidInput) {
    Error error;
    error.category = category;
    error.message = std::move(message);
    return error;
}

std::string fnv_hex(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string hex(16, '0');
    for (int index = 15; index >= 0; --index) {
        hex[static_cast<std::size_t>(index)] = digits[hash & 0xF];
        hash >>= 4;
    }
    return hex;
}

bool lower_alpha(char character) {
    return character >= 'a' && character <= 'z';
}

} // namespace

const char *name(PluginKind kind) noexcept {
    switch (kind) {
    case PluginKind::FixtureGenerator:
        return "fixture-generator";
    case PluginKind::Oracle:
        return "oracle";
    case PluginKind::Measurement:
        return "measurement";
    case PluginKind::Analysis:
        return "analysis";
    case PluginKind::Predictor:
        return "predictor";
    case PluginKind::Reporter:
        return "reporter";
    }
    return "unknown";
}

std::optional<PluginKind> parse_plugin_kind(std::string_view text) noexcept {
    if (text == "fixture-generator" || text == "fixture")
        return PluginKind::FixtureGenerator;
    if (text == "oracle" || text == "judge")
        return PluginKind::Oracle;
    if (text == "measurement" || text == "measure")
        return PluginKind::Measurement;
    if (text == "analysis" || text == "analyze")
        return PluginKind::Analysis;
    if (text == "predictor" || text == "predict")
        return PluginKind::Predictor;
    if (text == "reporter" || text == "report")
        return PluginKind::Reporter;
    return std::nullopt;
}

bool valid_plugin_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 128)
        return false;
    if (id.find('.') == std::string_view::npos)
        return false;
    bool previous_dot = true; // a leading dot is invalid
    for (char character : id) {
        const bool allowed = lower_alpha(character) ||
                             (character >= '0' && character <= '9') || character == '_' ||
                             character == '.';
        if (!allowed)
            return false;
        if (character == '.') {
            if (previous_dot)
                return false;
            previous_dot = true;
            continue;
        }
        previous_dot = false;
    }
    return !previous_dot;
}

const char *name(HookKind kind) noexcept {
    switch (kind) {
    case HookKind::Configure:
        return "configure";
    case HookKind::Generate:
        return "generate";
    case HookKind::Judge:
        return "judge";
    case HookKind::Measure:
        return "measure";
    case HookKind::Analyze:
        return "analyze";
    case HookKind::Predict:
        return "predict";
    case HookKind::Report:
        return "report";
    }
    return "unknown";
}

std::optional<HookKind> parse_hook_kind(std::string_view text) noexcept {
    if (text == "configure")
        return HookKind::Configure;
    if (text == "generate")
        return HookKind::Generate;
    if (text == "judge")
        return HookKind::Judge;
    if (text == "measure")
        return HookKind::Measure;
    if (text == "analyze")
        return HookKind::Analyze;
    if (text == "predict")
        return HookKind::Predict;
    if (text == "report")
        return HookKind::Report;
    return std::nullopt;
}

const char *name(HookComposition composition) noexcept {
    switch (composition) {
    case HookComposition::LastWins:
        return "last-wins";
    case HookComposition::AllMustPass:
        return "all-must-pass";
    case HookComposition::FirstPass:
        return "first-pass";
    }
    return "unknown";
}

std::optional<HookComposition> parse_hook_composition(std::string_view text) noexcept {
    if (text == "last-wins" || text == "last")
        return HookComposition::LastWins;
    if (text == "all-must-pass" || text == "all")
        return HookComposition::AllMustPass;
    if (text == "first-pass" || text == "first")
        return HookComposition::FirstPass;
    return std::nullopt;
}

bool valid_profile_id(std::string_view stable) noexcept {
    if (stable.empty() || stable.size() > 64)
        return false;
    if (!lower_alpha(stable.front()) && !(stable.front() >= '0' && stable.front() <= '9'))
        return false;
    for (char character : stable) {
        const bool allowed = lower_alpha(character) ||
                             (character >= '0' && character <= '9') || character == '_' ||
                             character == '.' || character == '-';
        if (!allowed)
            return false;
    }
    return stable != "." && stable != "..";
}

Result<ProfileId> make_profile_id(std::string_view display) {
    if (display.empty())
        return contract_error("profile display name must not be empty");
    std::string stable;
    stable.reserve(display.size());
    bool previous_separator = false;
    for (char character : display) {
        const unsigned char byte = static_cast<unsigned char>(character);
        char mapped = static_cast<char>(std::tolower(byte));
        const bool allowed = (mapped >= 'a' && mapped <= 'z') ||
                             (mapped >= '0' && mapped <= '9') || mapped == '_' ||
                             mapped == '.';
        if (!allowed) {
            if (!previous_separator && !stable.empty()) {
                stable.push_back('-');
                previous_separator = true;
            }
            continue;
        }
        previous_separator = false;
        stable.push_back(mapped);
    }
    while (!stable.empty() && (stable.back() == '-' || stable.back() == '.'))
        stable.pop_back();
    if (stable.empty())
        return contract_error("profile display name has no usable characters");
    if (!lower_alpha(stable.front()) && !(stable.front() >= '0' && stable.front() <= '9'))
        stable.insert(stable.begin(), 'p');
    if (!valid_profile_id(stable))
        return contract_error("derived profile id is not valid: " + stable);
    ProfileId id;
    id.stable = std::move(stable);
    id.display = std::string(display);
    return id;
}

Result<ExecutionFingerprint> fingerprint_execution(const FingerprintInput &input) {
    Json components = Json::object();
    components.set("executable", Json(input.executable_digest));
    Json argv = Json::array();
    for (const auto &argument : input.argv)
        argv.push(Json(argument));
    components.set("argv", std::move(argv));
    components.set("fixture", Json(input.fixture_digest));
    components.set("runner", Json(input.runner_digest));
    Json plugins = Json::array();
    for (const auto &id : input.plugin_ids)
        plugins.push(Json(id));
    components.set("plugins", std::move(plugins));
    components.set("seed", Json(static_cast<std::int64_t>(input.seed)));
    components.set("oracle", Json(input.oracle));
    components.set("measurement", Json(input.measurement_policy));
    components.set("compiler", Json(input.compiler_configuration));
    components.set("target_abi", Json(input.target_abi));
    components.set("environment", Json(input.environment_digest));

    ExecutionFingerprint fingerprint;
    fingerprint.components = components;
    fingerprint.digest = fnv_hex(components.dump());
    fingerprint.valid = true;
    return fingerprint;
}

const char *name(FactKind kind) noexcept {
    switch (kind) {
    case FactKind::Language:
        return "language";
    case FactKind::Semantics:
        return "semantics";
    case FactKind::Prerequisite:
        return "prerequisite";
    case FactKind::Target:
        return "target";
    case FactKind::Pass:
        return "pass";
    case FactKind::Exclusion:
        return "exclusion";
    case FactKind::Evidence:
        return "evidence";
    }
    return "unknown";
}

std::optional<FactKind> parse_fact_kind(std::string_view text) noexcept {
    if (text == "language")
        return FactKind::Language;
    if (text == "semantics")
        return FactKind::Semantics;
    if (text == "prerequisite")
        return FactKind::Prerequisite;
    if (text == "target")
        return FactKind::Target;
    if (text == "pass")
        return FactKind::Pass;
    if (text == "exclusion")
        return FactKind::Exclusion;
    if (text == "evidence")
        return FactKind::Evidence;
    return std::nullopt;
}

const char *name(RecommendationSource source) noexcept {
    switch (source) {
    case RecommendationSource::PatternLibrary:
        return "pattern-library";
    case RecommendationSource::Solver:
        return "solver";
    case RecommendationSource::Model:
        return "model";
    }
    return "unknown";
}

std::optional<RecommendationSource> parse_recommendation_source(std::string_view text) noexcept {
    if (text == "pattern-library" || text == "pattern")
        return RecommendationSource::PatternLibrary;
    if (text == "solver" || text == "satie")
        return RecommendationSource::Solver;
    if (text == "model" || text == "ai")
        return RecommendationSource::Model;
    return std::nullopt;
}

const char *name(RecommendationStatus status) noexcept {
    switch (status) {
    case RecommendationStatus::Feasible:
        return "feasible";
    case RecommendationStatus::Rejected:
        return "rejected";
    case RecommendationStatus::NeedsEvidence:
        return "needs-evidence";
    case RecommendationStatus::Unknown:
        return "unknown";
    case RecommendationStatus::BudgetExhausted:
        return "budget-exhausted";
    }
    return "unknown";
}

const char *name(SolverOutcome outcome) noexcept {
    switch (outcome) {
    case SolverOutcome::Satisfiable:
        return "satisfiable";
    case SolverOutcome::Unsatisfiable:
        return "unsatisfiable";
    case SolverOutcome::Unknown:
        return "unknown";
    case SolverOutcome::BudgetExhausted:
        return "budget-exhausted";
    }
    return "unknown";
}

Result<void> validate_facts(const OracleFacts &facts) {
    if (facts.language.empty())
        return contract_error("oracle facts must name the source language");
    if (facts.semantics.empty())
        return contract_error("oracle facts must describe the semantics model");
    std::vector<std::string> seen;
    for (const auto &fact : facts.facts) {
        if (fact.id.empty())
            return contract_error("oracle fact without an id");
        if (fact.statement.empty())
            return contract_error("oracle fact '" + fact.id + "' has no statement");
        if (std::find(seen.begin(), seen.end(), fact.id) != seen.end())
            return contract_error("duplicate oracle fact id '" + fact.id + "'");
        seen.push_back(fact.id);
    }
    return {};
}

} // namespace v1
} // namespace moonstone::mahak
