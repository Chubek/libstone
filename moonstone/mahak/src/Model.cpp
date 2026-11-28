#include "mahak/Model.hpp"

#include <sstream>

namespace moonstone::mahak {
inline namespace v1 {

const char *name(TargetKind value) noexcept {
    switch (value) {
    case TargetKind::Compiler:
        return "compiler";
    case TargetKind::Interpreter:
        return "interpreter";
    case TargetKind::VirtualMachine:
        return "vm";
    case TargetKind::Executable:
        return "executable";
    case TargetKind::Library:
        return "library";
    case TargetKind::CustomCommand:
        return "custom-command";
    }
    return "unknown";
}

const char *name(StageKind value) noexcept {
    switch (value) {
    case StageKind::Setup:
        return "setup";
    case StageKind::Compile:
        return "compile";
    case StageKind::Link:
        return "link";
    case StageKind::Validate:
        return "validate";
    case StageKind::Execute:
        return "execute";
    case StageKind::Teardown:
        return "teardown";
    }
    return "unknown";
}

const char *name(ExecutionMode value) noexcept {
    switch (value) {
    case ExecutionMode::CompileOnly:
        return "compile-only";
    case ExecutionMode::LinkOnly:
        return "link-only";
    case ExecutionMode::ExecuteOnly:
        return "execute-only";
    case ExecutionMode::EndToEnd:
        return "end-to-end";
    case ExecutionMode::Startup:
        return "startup";
    case ExecutionMode::Throughput:
        return "throughput";
    case ExecutionMode::Scalability:
        return "scalability";
    }
    return "unknown";
}

const char *name(MeasurementMode value) noexcept {
    switch (value) {
    case MeasurementMode::ColdProcess:
        return "cold-process";
    case MeasurementMode::WarmProcess:
        return "warm-process";
    case MeasurementMode::PersistentProcess:
        return "persistent-process";
    case MeasurementMode::Concurrent:
        return "concurrent";
    }
    return "unknown";
}

const char *name(MetricDirection value) noexcept {
    switch (value) {
    case MetricDirection::LowerIsBetter:
        return "lower";
    case MetricDirection::HigherIsBetter:
        return "higher";
    case MetricDirection::Neutral:
        return "neutral";
    }
    return "unknown";
}

const char *name(Validity value) noexcept {
    switch (value) {
    case Validity::Valid:
        return "valid";
    case Validity::InvalidOutput:
        return "invalid-output";
    case Validity::NonZeroExit:
        return "non-zero-exit";
    case Validity::TimedOut:
        return "timed-out";
    case Validity::Crashed:
        return "crashed";
    case Validity::Cancelled:
        return "cancelled";
    case Validity::Unsupported:
        return "unsupported";
    case Validity::SetupFailed:
        return "setup-failed";
    case Validity::MeasurementFailed:
        return "measurement-failed";
    }
    return "unknown";
}

std::optional<TargetKind> parse_target_kind(std::string_view text) noexcept {
    if (text == "compiler")
        return TargetKind::Compiler;
    if (text == "interpreter")
        return TargetKind::Interpreter;
    if (text == "vm" || text == "virtual-machine" || text == "virtual_machine")
        return TargetKind::VirtualMachine;
    if (text == "executable" || text == "native")
        return TargetKind::Executable;
    if (text == "library" || text == "lib")
        return TargetKind::Library;
    if (text == "custom-command" || text == "command" || text == "custom")
        return TargetKind::CustomCommand;
    return std::nullopt;
}

std::optional<StageKind> parse_stage_kind(std::string_view text) noexcept {
    if (text == "setup")
        return StageKind::Setup;
    if (text == "compile" || text == "build")
        return StageKind::Compile;
    if (text == "link")
        return StageKind::Link;
    if (text == "validate")
        return StageKind::Validate;
    if (text == "execute" || text == "run")
        return StageKind::Execute;
    if (text == "teardown" || text == "cleanup")
        return StageKind::Teardown;
    return std::nullopt;
}

std::optional<ExecutionMode> parse_execution_mode(std::string_view text) noexcept {
    if (text == "compile-only" || text == "compile")
        return ExecutionMode::CompileOnly;
    if (text == "link-only" || text == "link")
        return ExecutionMode::LinkOnly;
    if (text == "execute-only" || text == "execute" || text == "run")
        return ExecutionMode::ExecuteOnly;
    if (text == "end-to-end" || text == "e2e")
        return ExecutionMode::EndToEnd;
    if (text == "startup")
        return ExecutionMode::Startup;
    if (text == "throughput")
        return ExecutionMode::Throughput;
    if (text == "scalability")
        return ExecutionMode::Scalability;
    return std::nullopt;
}

std::optional<MeasurementMode> parse_measurement_mode(std::string_view text) noexcept {
    if (text == "cold-process" || text == "cold")
        return MeasurementMode::ColdProcess;
    if (text == "warm-process" || text == "warm")
        return MeasurementMode::WarmProcess;
    if (text == "persistent-process" || text == "persistent")
        return MeasurementMode::PersistentProcess;
    if (text == "concurrent")
        return MeasurementMode::Concurrent;
    return std::nullopt;
}

std::optional<MetricDirection> parse_metric_direction(std::string_view text) noexcept {
    if (text == "lower" || text == "lower-is-better")
        return MetricDirection::LowerIsBetter;
    if (text == "higher" || text == "higher-is-better")
        return MetricDirection::HigherIsBetter;
    if (text == "neutral")
        return MetricDirection::Neutral;
    return std::nullopt;
}

std::string expand_placeholders(std::string_view input,
                                const std::map<std::string, std::string> &variables,
                                bool *unresolved) {
    std::string result;
    result.reserve(input.size());
    bool missing = false;
    std::size_t position = 0;
    while (position < input.size()) {
        const char current = input[position];
        if (current != '{') {
            result.push_back(current);
            ++position;
            continue;
        }
        const std::size_t end = input.find('}', position + 1);
        if (end == std::string_view::npos) {
            result.append(input.substr(position));
            break;
        }
        const std::string_view key = input.substr(position + 1, end - position - 1);
        const auto found = variables.find(std::string(key));
        if (found == variables.end()) {
            missing = true;
            result.append(input.substr(position, end - position + 1));
        } else {
            result += found->second;
        }
        position = end + 1;
    }
    if (unresolved)
        *unresolved = missing;
    return result;
}

std::string format_parameter_value(const Json &value) {
    if (const auto *text = value.as_string())
        return *text;
    if (const auto integer = value.as_int64())
        return std::to_string(*integer);
    if (const auto number = value.as_double()) {
        std::ostringstream stream;
        stream.precision(17);
        stream << *number;
        return stream.str();
    }
    if (const auto boolean = value.as_bool())
        return *boolean ? "true" : "false";
    if (value.is_null())
        return "null";
    return value.dump();
}

std::string join_argv(const std::vector<std::string> &argv) {
    std::string result;
    for (const auto &argument : argv) {
        if (!result.empty())
            result.push_back(' ');
        // Quoting is for diagnostics only; it is never re-parsed into argv.
        const bool safe = !argument.empty() &&
                          argument.find_first_of(" \t\n\"'\\$`;&|<>(){}[]*?!#") ==
                              std::string::npos;
        if (safe) {
            result += argument;
        } else {
            result.push_back('\'');
            for (char c : argument) {
                if (c == '\'')
                    result += "'\\''";
                else
                    result.push_back(c);
            }
            result.push_back('\'');
        }
    }
    return result;
}

std::string metric_direction_name(MetricDirection direction) noexcept {
    return name(direction);
}

} // namespace v1
} // namespace moonstone::mahak
