#include "mahak/Manifest.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace moonstone::mahak {
inline namespace v1 {
namespace {

using Object = Json::Object;

Error invalid(std::string_view context, std::string_view message,
              ErrorCategory category = ErrorCategory::InvalidInput) {
    Error error;
    error.category = category;
    error.message = std::string(context) + ": " + std::string(message);
    return error;
}

std::string join_path(std::string_view base, std::string_view key) {
    if (base.empty())
        return std::string(key);
    return std::string(base) + "." + std::string(key);
}

std::string index_path(std::string_view base, std::size_t index) {
    return std::string(base) + "[" + std::to_string(index) + "]";
}

const Json *field(const Object &object, std::string_view key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

Result<void> check_keys(const Object &object, std::string_view path,
                        std::initializer_list<std::string_view> allowed) {
    for (const auto &entry : object) {
        bool known = false;
        for (std::string_view candidate : allowed) {
            if (entry.first == candidate) {
                known = true;
                break;
            }
        }
        if (!known)
            return invalid(join_path(path, entry.first), "unknown key");
    }
    return {};
}

Result<std::string> required_string(const Object &object, std::string_view key,
                                    std::string_view path) {
    const Json *value = field(object, key);
    if (value == nullptr)
        return invalid(path, "'" + std::string(key) + "' is required");
    const std::string *text = value->as_string();
    if (text == nullptr)
        return invalid(join_path(path, key), "expected a string");
    if (text->empty())
        return invalid(join_path(path, key), "must not be empty");
    return *text;
}

Result<std::string> optional_string(const Object &object, std::string_view key,
                                    std::string_view path, std::string fallback) {
    const Json *value = field(object, key);
    if (value == nullptr)
        return fallback;
    const std::string *text = value->as_string();
    if (text == nullptr)
        return invalid(join_path(path, key), "expected a string");
    return *text;
}

Result<std::uint64_t> optional_uint(const Object &object, std::string_view key,
                                    std::string_view path, std::uint64_t fallback) {
    const Json *value = field(object, key);
    if (value == nullptr)
        return fallback;
    const auto number = value->as_uint64();
    if (!number)
        return invalid(join_path(path, key), "expected a non-negative integer");
    return *number;
}

Result<std::int64_t> optional_int(const Object &object, std::string_view key,
                                  std::string_view path, std::int64_t fallback) {
    const Json *value = field(object, key);
    if (value == nullptr)
        return fallback;
    const auto number = value->as_int64();
    if (!number)
        return invalid(join_path(path, key), "expected an integer");
    return *number;
}

Result<double> optional_double(const Object &object, std::string_view key,
                               std::string_view path, double fallback) {
    const Json *value = field(object, key);
    if (value == nullptr)
        return fallback;
    if (!value->is_number())
        return invalid(join_path(path, key), "expected a number");
    const auto number = value->as_double();
    if (!number)
        return invalid(join_path(path, key), "expected a finite number");
    return *number;
}

Result<bool> optional_bool(const Object &object, std::string_view key,
                           std::string_view path, bool fallback) {
    const Json *value = field(object, key);
    if (value == nullptr)
        return fallback;
    const auto boolean = value->as_bool();
    if (!boolean)
        return invalid(join_path(path, key), "expected a boolean");
    return *boolean;
}

Result<const Json::Array *> array_field(const Object &object, std::string_view key,
                                        std::string_view path, bool required) {
    const Json *value = field(object, key);
    if (value == nullptr) {
        if (required)
            return invalid(path, "'" + std::string(key) + "' is required");
        return static_cast<const Json::Array *>(nullptr);
    }
    const Json::Array *array = value->as_array();
    if (array == nullptr)
        return invalid(join_path(path, key), "expected an array");
    return array;
}

Result<std::vector<EnvironmentVariable>> parse_environment(const Json &owner,
                                                           std::string_view path) {
    const Json::Array *array = owner.as_array();
    if (array == nullptr)
        return invalid(path, "expected an array of environment entries");
    std::vector<EnvironmentVariable> variables;
    for (std::size_t index = 0; index < array->size(); ++index) {
        const std::string at = index_path(path, index);
        if (!(*array)[index].is_object())
            return invalid(at, "expected an object");
        const Object &entry = *(*array)[index].as_object();
        if (auto keys = check_keys(entry, at, {"name", "value", "overwrite"}); !keys)
            return keys.error();
        EnvironmentVariable variable;
        auto name = required_string(entry, "name", at);
        if (!name)
            return name.error();
        variable.name = name.value();
        auto value = optional_string(entry, "value", at, "");
        if (!value)
            return value.error();
        variable.value = value.value();
        auto overwrite = optional_bool(entry, "overwrite", at, true);
        if (!overwrite)
            return overwrite.error();
        variable.overwrite = overwrite.value();
        variables.push_back(std::move(variable));
    }
    return variables;
}

Result<CommandSpec> parse_command(const Json &value, std::string_view path) {
    if (!value.is_object())
        return invalid(path, "expected a command object");
    const Object &object = *value.as_object();
    if (auto keys = check_keys(object, path,
                               {"argv", "working_directory", "environment", "stdin_data",
                                "stdin_file", "artifacts", "timeout_ms"});
        !keys)
        return keys.error();
    CommandSpec command;
    auto argv = array_field(object, "argv", path, true);
    if (!argv)
        return argv.error();
    if (argv.value()->empty())
        return invalid(join_path(path, "argv"), "must not be empty");
    for (std::size_t index = 0; index < argv.value()->size(); ++index) {
        const auto *element = (*argv.value())[index].as_string();
        if (element == nullptr)
            return invalid(index_path(join_path(path, "argv"), index),
                           "expected a string");
        if (element->empty())
            return invalid(index_path(join_path(path, "argv"), index),
                           "argument must not be empty");
        command.argv.push_back(*element);
    }
    auto working = optional_string(object, "working_directory", path, "");
    if (!working)
        return working.error();
    command.working_directory = working.value();
    if (const Json *environment = field(object, "environment")) {
        auto parsed = parse_environment(*environment, join_path(path, "environment"));
        if (!parsed)
            return parsed.error();
        command.environment = parsed.value();
    }
    auto stdin_data = optional_string(object, "stdin_data", path, "");
    if (!stdin_data)
        return stdin_data.error();
    command.stdin_data = stdin_data.value();
    auto stdin_file = optional_string(object, "stdin_file", path, "");
    if (!stdin_file)
        return stdin_file.error();
    command.stdin_file = stdin_file.value();
    auto artifacts = array_field(object, "artifacts", path, false);
    if (!artifacts)
        return artifacts.error();
    if (artifacts.value() != nullptr) {
        for (std::size_t index = 0; index < artifacts.value()->size(); ++index) {
            const auto *element = (*artifacts.value())[index].as_string();
            if (element == nullptr)
                return invalid(index_path(join_path(path, "artifacts"), index),
                               "expected a string");
            command.artifacts.push_back(*element);
        }
    }
    auto timeout = optional_int(object, "timeout_ms", path, 0);
    if (!timeout)
        return timeout.error();
    if (timeout.value() < 0)
        return invalid(join_path(path, "timeout_ms"), "must not be negative");
    command.timeout_ms = timeout.value();
    return command;
}

Result<StageSpec> parse_stage(const Json &value, std::string_view path) {
    if (!value.is_object())
        return invalid(path, "expected a stage object");
    const Object &object = *value.as_object();
    if (auto keys = check_keys(object, path, {"kind", "commands", "optional"}); !keys)
        return keys.error();
    StageSpec stage;
    auto kind = required_string(object, "kind", path);
    if (!kind)
        return kind.error();
    const auto parsed = parse_stage_kind(kind.value());
    if (!parsed)
        return invalid(join_path(path, "kind"), "unknown stage kind '" + kind.value() + "'");
    stage.kind = *parsed;
    auto commands = array_field(object, "commands", path, true);
    if (!commands)
        return commands.error();
    if (commands.value()->empty())
        return invalid(join_path(path, "commands"), "must not be empty");
    for (std::size_t index = 0; index < commands.value()->size(); ++index) {
        auto command = parse_command((*commands.value())[index],
                                     index_path(join_path(path, "commands"), index));
        if (!command)
            return command.error();
        stage.commands.push_back(command.value());
    }
    auto optional_flag = optional_bool(object, "optional", path, false);
    if (!optional_flag)
        return optional_flag.error();
    stage.optional = optional_flag.value();
    return stage;
}

Result<std::vector<StageSpec>> parse_stages(const Object &object, std::string_view path) {
    auto array = array_field(object, "stages", path, false);
    if (!array)
        return array.error();
    std::vector<StageSpec> stages;
    if (array.value() == nullptr)
        return stages;
    for (std::size_t index = 0; index < array.value()->size(); ++index) {
        auto stage = parse_stage((*array.value())[index], index_path(join_path(path, "stages"), index));
        if (!stage)
            return stage.error();
        stages.push_back(stage.value());
    }
    return stages;
}

Result<std::vector<ParameterSpec>> parse_parameters(const Object &object,
                                                    std::string_view path) {
    auto array = array_field(object, "parameters", path, false);
    if (!array)
        return array.error();
    std::vector<ParameterSpec> parameters;
    if (array.value() == nullptr)
        return parameters;
    std::set<std::string> names;
    for (std::size_t index = 0; index < array.value()->size(); ++index) {
        const std::string at = index_path(join_path(path, "parameters"), index);
        const Json &value = (*array.value())[index];
        if (!value.is_object())
            return invalid(at, "expected an object");
        const Object &entry = *value.as_object();
        if (auto keys = check_keys(entry, at, {"name", "values"}); !keys)
            return keys.error();
        ParameterSpec parameter;
        auto name = required_string(entry, "name", at);
        if (!name)
            return name.error();
        parameter.name = name.value();
        if (!names.insert(parameter.name).second)
            return invalid(join_path(at, "name"),
                           "duplicate parameter '" + parameter.name + "'");
        auto values = array_field(entry, "values", at, true);
        if (!values)
            return values.error();
        if (values.value()->empty())
            return invalid(join_path(at, "values"), "must contain at least one value");
        parameter.values = Json(*values.value());
        parameters.push_back(std::move(parameter));
    }
    return parameters;
}

Result<OracleSpec> parse_oracle(const Object &object, std::string_view path) {
    if (auto keys = check_keys(object, path,
                               {"kind", "expected_exit", "expected", "pattern", "field",
                                "tolerance"});
        !keys)
        return keys.error();
    OracleSpec oracle;
    auto kind = optional_string(object, "kind", path, "exit-code");
    if (!kind)
        return kind.error();
    const std::string &text = kind.value();
    if (text == "none")
        oracle.kind = OracleKind::None;
    else if (text == "exit-code")
        oracle.kind = OracleKind::ExitCode;
    else if (text == "stdout-exact")
        oracle.kind = OracleKind::StdoutExact;
    else if (text == "stdout-contains")
        oracle.kind = OracleKind::StdoutContains;
    else if (text == "regex")
        oracle.kind = OracleKind::Regex;
    else if (text == "checksum")
        oracle.kind = OracleKind::Checksum;
    else if (text == "tolerance")
        oracle.kind = OracleKind::Tolerance;
    else if (text == "json-field")
        oracle.kind = OracleKind::JsonField;
    else
        return invalid(join_path(path, "kind"), "unknown oracle kind '" + text + "'");
    auto expected_exit = optional_int(object, "expected_exit", path, 0);
    if (!expected_exit)
        return expected_exit.error();
    oracle.expected_exit = static_cast<int>(expected_exit.value());
    auto expected = optional_string(object, "expected", path, "");
    if (!expected)
        return expected.error();
    oracle.expected = expected.value();
    auto pattern = optional_string(object, "pattern", path, "");
    if (!pattern)
        return pattern.error();
    oracle.pattern = pattern.value();
    auto field_name = optional_string(object, "field", path, "");
    if (!field_name)
        return field_name.error();
    oracle.field = field_name.value();
    auto tolerance = optional_double(object, "tolerance", path, 1e-9);
    if (!tolerance)
        return tolerance.error();
    oracle.tolerance = tolerance.value();
    switch (oracle.kind) {
    case OracleKind::Regex:
        if (oracle.pattern.empty())
            return invalid(join_path(path, "pattern"), "regex oracle requires a pattern");
        break;
    case OracleKind::Checksum:
    case OracleKind::StdoutExact:
    case OracleKind::StdoutContains:
        if (oracle.expected.empty())
            return invalid(join_path(path, "expected"),
                           "this oracle kind requires an expected value");
        break;
    case OracleKind::JsonField:
        if (oracle.field.empty())
            return invalid(join_path(path, "field"), "json-field oracle requires a field");
        break;
    case OracleKind::None:
    case OracleKind::ExitCode:
    case OracleKind::Tolerance:
        break;
    }
    return oracle;
}

Result<std::vector<MetricSpec>> parse_metrics(const Object &object, std::string_view path) {
    auto array = array_field(object, "metrics", path, false);
    if (!array)
        return array.error();
    std::vector<MetricSpec> metrics;
    if (array.value() == nullptr)
        return metrics;
    std::set<std::string> names;
    for (std::size_t index = 0; index < array.value()->size(); ++index) {
        const std::string at = index_path(join_path(path, "metrics"), index);
        const Json &value = (*array.value())[index];
        if (!value.is_object())
            return invalid(at, "expected an object");
        const Object &entry = *value.as_object();
        if (auto keys = check_keys(entry, at, {"name", "unit", "direction", "primary"}); !keys)
            return keys.error();
        MetricSpec metric;
        auto name = required_string(entry, "name", at);
        if (!name)
            return name.error();
        metric.name = name.value();
        if (!names.insert(metric.name).second)
            return invalid(join_path(at, "name"), "duplicate metric '" + metric.name + "'");
        auto unit = required_string(entry, "unit", at);
        if (!unit)
            return unit.error();
        metric.unit = unit.value();
        auto direction = optional_string(entry, "direction", at, "lower");
        if (!direction)
            return direction.error();
        const auto parsed = parse_metric_direction(direction.value());
        if (!parsed)
            return invalid(join_path(at, "direction"),
                           "unknown metric direction '" + direction.value() + "'");
        metric.direction = *parsed;
        auto primary = optional_bool(entry, "primary", at, false);
        if (!primary)
            return primary.error();
        metric.primary = primary.value();
        metrics.push_back(std::move(metric));
    }
    return metrics;
}

Result<SamplingPolicy> parse_sampling(const Object &object, std::string_view path,
                                      const SamplingPolicy &fallback) {
    if (auto keys = check_keys(object, path,
                               {"warmup", "repetitions", "min_duration_ns", "max_duration_ns",
                                "max_repetitions", "adaptive", "target_relative_stddev",
                                "measurement", "interleave", "seed"});
        !keys)
        return keys.error();
    SamplingPolicy policy = fallback;
    auto warmup = optional_uint(object, "warmup", path, policy.warmup);
    if (!warmup)
        return warmup.error();
    policy.warmup = static_cast<std::uint32_t>(warmup.value());
    auto repetitions = optional_uint(object, "repetitions", path, policy.repetitions);
    if (!repetitions)
        return repetitions.error();
    if (repetitions.value() == 0)
        return invalid(join_path(path, "repetitions"), "must be at least 1");
    policy.repetitions = static_cast<std::uint32_t>(repetitions.value());
    auto min_duration = optional_uint(object, "min_duration_ns", path, policy.min_duration_ns);
    if (!min_duration)
        return min_duration.error();
    policy.min_duration_ns = min_duration.value();
    auto max_duration = optional_uint(object, "max_duration_ns", path, policy.max_duration_ns);
    if (!max_duration)
        return max_duration.error();
    policy.max_duration_ns = max_duration.value();
    auto max_repetitions =
        optional_uint(object, "max_repetitions", path, policy.max_repetitions);
    if (!max_repetitions)
        return max_repetitions.error();
    policy.max_repetitions = static_cast<std::uint32_t>(max_repetitions.value());
    auto adaptive = optional_bool(object, "adaptive", path, policy.adaptive);
    if (!adaptive)
        return adaptive.error();
    policy.adaptive = adaptive.value();
    auto stddev = optional_double(object, "target_relative_stddev", path,
                                  policy.target_relative_stddev);
    if (!stddev)
        return stddev.error();
    if (!(stddev.value() > 0.0))
        return invalid(join_path(path, "target_relative_stddev"), "must be positive");
    policy.target_relative_stddev = stddev.value();
    auto measurement = optional_string(object, "measurement", path,
                                       name(policy.measurement));
    if (!measurement)
        return measurement.error();
    const auto mode = parse_measurement_mode(measurement.value());
    if (!mode)
        return invalid(join_path(path, "measurement"),
                       "unknown measurement mode '" + measurement.value() + "'");
    policy.measurement = *mode;
    auto interleave = optional_bool(object, "interleave", path, policy.interleave);
    if (!interleave)
        return interleave.error();
    policy.interleave = interleave.value();
    auto seed = optional_uint(object, "seed", path, policy.seed);
    if (!seed)
        return seed.error();
    policy.seed = seed.value();
    if (policy.min_duration_ns != 0 && policy.max_duration_ns != 0 &&
        policy.max_duration_ns < policy.min_duration_ns)
        return invalid(join_path(path, "max_duration_ns"),
                       "must be greater than or equal to min_duration_ns");
    return policy;
}

Result<std::vector<FixtureSpec>> parse_fixtures(const Object &object, std::string_view path) {
    auto array = array_field(object, "fixtures", path, false);
    if (!array)
        return array.error();
    std::vector<FixtureSpec> fixtures;
    if (array.value() == nullptr)
        return fixtures;
    std::set<std::string> names;
    for (std::size_t index = 0; index < array.value()->size(); ++index) {
        const std::string at = index_path(join_path(path, "fixtures"), index);
        const Json &value = (*array.value())[index];
        if (!value.is_object())
            return invalid(at, "expected an object");
        const Object &entry = *value.as_object();
        if (auto keys = check_keys(entry, at, {"name", "path", "target", "required"}); !keys)
            return keys.error();
        FixtureSpec fixture;
        auto name = required_string(entry, "name", at);
        if (!name)
            return name.error();
        fixture.name = name.value();
        if (!names.insert(fixture.name).second)
            return invalid(join_path(at, "name"),
                           "duplicate fixture '" + fixture.name + "'");
        auto source = required_string(entry, "path", at);
        if (!source)
            return source.error();
        fixture.path = source.value();
        auto target_name = optional_string(entry, "target", at, fixture.name);
        if (!target_name)
            return target_name.error();
        fixture.target_name = target_name.value();
        auto required = optional_bool(entry, "required", at, true);
        if (!required)
            return required.error();
        fixture.required = required.value();
        fixtures.push_back(std::move(fixture));
    }
    return fixtures;
}

Result<TargetSpec> parse_target(const Json &value, std::string_view path) {
    if (!value.is_object())
        return invalid(path, "expected a target object");
    const Object &object = *value.as_object();
    if (auto keys = check_keys(object, path,
                               {"id", "kind", "description", "stages", "execute",
                                "parameters", "environment"});
        !keys)
        return keys.error();
    TargetSpec target;
    auto id = required_string(object, "id", path);
    if (!id)
        return id.error();
    target.id = id.value();
    auto kind = required_string(object, "kind", path);
    if (!kind)
        return kind.error();
    const auto parsed_kind = parse_target_kind(kind.value());
    if (!parsed_kind)
        return invalid(join_path(path, "kind"), "unknown target kind '" + kind.value() + "'");
    target.kind = *parsed_kind;
    auto description = optional_string(object, "description", path, "");
    if (!description)
        return description.error();
    target.description = description.value();
    auto stages = parse_stages(object, path);
    if (!stages)
        return stages.error();
    target.stages = stages.value();
    if (const Json *execute = field(object, "execute")) {
        auto command = parse_command(*execute, join_path(path, "execute"));
        if (!command)
            return command.error();
        target.execute = command.value();
    }
    auto parameters = parse_parameters(object, path);
    if (!parameters)
        return parameters.error();
    target.parameters = parameters.value();
    if (const Json *environment = field(object, "environment")) {
        auto parsed = parse_environment(*environment, join_path(path, "environment"));
        if (!parsed)
            return parsed.error();
        target.environment = parsed.value();
    }
    return target;
}

Result<BenchmarkSpec> parse_benchmark(const Json &value, std::string_view path,
                                      const SamplingPolicy &defaults) {
    if (!value.is_object())
        return invalid(path, "expected a benchmark object");
    const Object &object = *value.as_object();
    if (auto keys = check_keys(object, path,
                               {"id", "description", "group", "tags", "modes", "fixtures",
                                "parameters", "stages", "execute", "oracle", "metrics",
                                "sampling", "work_directory"});
        !keys)
        return keys.error();
    BenchmarkSpec benchmark;
    auto id = required_string(object, "id", path);
    if (!id)
        return id.error();
    benchmark.id = id.value();
    auto description = optional_string(object, "description", path, "");
    if (!description)
        return description.error();
    benchmark.description = description.value();
    auto group = optional_string(object, "group", path, "");
    if (!group)
        return group.error();
    benchmark.group = group.value();
    auto tags = array_field(object, "tags", path, false);
    if (!tags)
        return tags.error();
    if (tags.value() != nullptr) {
        for (std::size_t index = 0; index < tags.value()->size(); ++index) {
            const auto *text = (*tags.value())[index].as_string();
            if (text == nullptr)
                return invalid(index_path(join_path(path, "tags"), index), "expected a string");
            benchmark.tags.push_back(*text);
        }
    }
    auto modes = array_field(object, "modes", path, false);
    if (!modes)
        return modes.error();
    if (modes.value() != nullptr) {
        std::set<ExecutionMode> seen;
        for (std::size_t index = 0; index < modes.value()->size(); ++index) {
            const auto *text = (*modes.value())[index].as_string();
            if (text == nullptr)
                return invalid(index_path(join_path(path, "modes"), index), "expected a string");
            const auto parsed = parse_execution_mode(*text);
            if (!parsed)
                return invalid(index_path(join_path(path, "modes"), index),
                               "unknown execution mode '" + *text + "'");
            if (seen.insert(*parsed).second)
                benchmark.modes.push_back(*parsed);
        }
    }
    auto fixtures = parse_fixtures(object, path);
    if (!fixtures)
        return fixtures.error();
    benchmark.fixtures = fixtures.value();
    auto parameters = parse_parameters(object, path);
    if (!parameters)
        return parameters.error();
    benchmark.parameters = parameters.value();
    auto stages = parse_stages(object, path);
    if (!stages)
        return stages.error();
    benchmark.stages = stages.value();
    if (const Json *execute = field(object, "execute")) {
        auto command = parse_command(*execute, join_path(path, "execute"));
        if (!command)
            return command.error();
        benchmark.execute = command.value();
    }
    if (const Json *oracle = field(object, "oracle")) {
        if (!oracle->is_object())
            return invalid(join_path(path, "oracle"), "expected an object");
        auto parsed_oracle = parse_oracle(*oracle->as_object(), join_path(path, "oracle"));
        if (!parsed_oracle)
            return parsed_oracle.error();
        benchmark.oracle = parsed_oracle.value();
    }
    // A benchmark without metrics still records the default wall/cpu/rss set;
    // MetricSpec entries only add named metrics and directions.
    auto metrics = parse_metrics(object, path);
    if (!metrics)
        return metrics.error();
    benchmark.metrics = metrics.value();
    benchmark.sampling = defaults;
    if (const Json *sampling = field(object, "sampling")) {
        if (!sampling->is_object())
            return invalid(join_path(path, "sampling"), "expected an object");
        auto parsed = parse_sampling(*sampling->as_object(), join_path(path, "sampling"),
                                     defaults);
        if (!parsed)
            return parsed.error();
        benchmark.sampling = parsed.value();
    }
    auto work = optional_string(object, "work_directory", path, "");
    if (!work)
        return work.error();
    benchmark.work_directory = work.value();
    return benchmark;
}

std::string sanitize_component(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out.push_back(safe ? c : '_');
    }
    return out.empty() ? std::string("case") : out;
}

bool stage_ordered(StageKind earlier, StageKind later) {
    return static_cast<int>(earlier) <= static_cast<int>(later);
}

bool mode_needs(ExecutionMode mode, StageKind stage) {
    switch (mode) {
    case ExecutionMode::CompileOnly:
        return static_cast<int>(stage) <= static_cast<int>(StageKind::Compile);
    case ExecutionMode::LinkOnly:
        return static_cast<int>(stage) <= static_cast<int>(StageKind::Link);
    case ExecutionMode::ExecuteOnly:
        return stage == StageKind::Execute;
    case ExecutionMode::EndToEnd:
    case ExecutionMode::Startup:
    case ExecutionMode::Throughput:
    case ExecutionMode::Scalability:
        return true;
    }
    return true;
}

std::string parent_directory(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string_view::npos)
        return ".";
    if (slash == 0)
        return "/";
    return std::string(path.substr(0, slash));
}

std::string absolute_join(std::string_view base, std::string_view relative) {
    if (!relative.empty() && relative.front() == '/')
        return std::string(relative);
    if (relative.empty())
        return std::string(base);
    if (base.empty())
        return std::string(relative);
    if (base.back() == '/')
        return std::string(base) + std::string(relative);
    return std::string(base) + "/" + std::string(relative);
}

Json digest_view(const Manifest &manifest) {
    Json view = Json::object();
    view.set("schema", Json(static_cast<std::int64_t>(manifest.schema)));
    view.set("id", Json(manifest.id));
    view.set("version", Json(manifest.version));
    view.set("seed", Json(static_cast<std::int64_t>(manifest.seed)));
    Json targets = Json::array();
    for (const auto &target : manifest.targets) {
        Json entry = Json::object();
        entry.set("id", Json(target.id));
        entry.set("kind", Json(name(target.kind)));
        Json parameters = Json::array();
        for (const auto &parameter : target.parameters) {
            Json item = Json::object();
            item.set("name", Json(parameter.name));
            item.set("values", parameter.values);
            parameters.push(std::move(item));
        }
        entry.set("parameters", std::move(parameters));
        targets.push(std::move(entry));
    }
    view.set("targets", std::move(targets));
    Json benchmarks = Json::array();
    for (const auto &benchmark : manifest.benchmarks) {
        Json entry = Json::object();
        entry.set("id", Json(benchmark.id));
        entry.set("group", Json(benchmark.group));
        Json parameters = Json::array();
        for (const auto &parameter : benchmark.parameters) {
            Json item = Json::object();
            item.set("name", Json(parameter.name));
            item.set("values", parameter.values);
            parameters.push(std::move(item));
        }
        entry.set("parameters", std::move(parameters));
        benchmarks.push(std::move(entry));
    }
    view.set("benchmarks", std::move(benchmarks));
    Json exclusions = Json::array();
    for (const auto &exclusion : manifest.exclusions) {
        Json entry = Json::object();
        entry.set("benchmark", Json(exclusion.benchmark));
        entry.set("target", Json(exclusion.target));
        exclusions.push(std::move(entry));
    }
    view.set("exclusions", std::move(exclusions));
    return view;
}

} // namespace

Result<Manifest> parse_manifest(std::string_view text, std::string_view source,
                                std::string base_directory) {
    auto parsed = Json::parse(text, source);
    if (!parsed)
        return parsed.error();
    if (!parsed.value().is_object())
        return invalid(std::string(source), "manifest must be a JSON object");
    const Object &object = *parsed.value().as_object();
    if (auto keys = check_keys(object, "",
                               {"schema", "id", "version", "description", "seed", "defaults",
                                "targets", "benchmarks", "exclusions"});
        !keys)
        return keys.error();

    Manifest manifest;
    auto schema = optional_int(object, "schema", "", mahak_manifest_schema_version);
    if (!schema)
        return schema.error();
    if (schema.value() != static_cast<std::int64_t>(mahak_manifest_schema_version))
        return invalid("schema", "unsupported manifest schema " + std::to_string(schema.value()));
    manifest.schema = static_cast<std::uint32_t>(schema.value());
    auto id = required_string(object, "id", "");
    if (!id)
        return id.error();
    manifest.id = id.value();
    auto version = optional_string(object, "version", "", "0.0.0");
    if (!version)
        return version.error();
    manifest.version = version.value();
    auto description = optional_string(object, "description", "", "");
    if (!description)
        return description.error();
    manifest.description = description.value();
    auto seed = optional_uint(object, "seed", "", 0);
    if (!seed)
        return seed.error();
    manifest.seed = seed.value();
    manifest.base_directory = std::move(base_directory);
    manifest.source_path = std::string(source);

    if (const Json *defaults = field(object, "defaults")) {
        if (!defaults->is_object())
            return invalid("defaults", "expected an object");
        // 'timeout_ms' is part of the documented defaults object but sampling
        // policy has no timeout field: per-command timeout_ms and
        // EngineOptions::timeout_ms are the timing authority in this build, so
        // the key is accepted (schema compatibility) and consumed by callers
        // that pass an engine-level default.
        Object sampling_defaults = *defaults->as_object();
        sampling_defaults.erase("timeout_ms");
        Json sampling_json(sampling_defaults);
        auto parsed_defaults =
            parse_sampling(*sampling_json.as_object(), "defaults", manifest.defaults);
        if (!parsed_defaults)
            return parsed_defaults.error();
        manifest.defaults = parsed_defaults.value();
    }

    auto targets = array_field(object, "targets", "", true);
    if (!targets)
        return targets.error();
    if (targets.value()->empty())
        return invalid("targets", "manifest must declare at least one target");
    std::set<std::string> target_ids;
    for (std::size_t index = 0; index < targets.value()->size(); ++index) {
        auto target = parse_target((*targets.value())[index], index_path("targets", index));
        if (!target)
            return target.error();
        if (!target_ids.insert(target.value().id).second)
            return invalid(index_path(join_path("targets", "id"), index),
                           "duplicate target id '" + target.value().id + "'");
        manifest.targets.push_back(target.value());
    }

    auto benchmarks = array_field(object, "benchmarks", "", true);
    if (!benchmarks)
        return benchmarks.error();
    if (benchmarks.value()->empty())
        return invalid("benchmarks", "manifest must declare at least one benchmark");
    std::set<std::string> benchmark_ids;
    for (std::size_t index = 0; index < benchmarks.value()->size(); ++index) {
        auto benchmark = parse_benchmark((*benchmarks.value())[index],
                                         index_path("benchmarks", index), manifest.defaults);
        if (!benchmark)
            return benchmark.error();
        if (!benchmark_ids.insert(benchmark.value().id).second)
            return invalid(index_path(join_path("benchmarks", "id"), index),
                           "duplicate benchmark id '" + benchmark.value().id + "'");
        manifest.benchmarks.push_back(benchmark.value());
    }

    auto exclusions = array_field(object, "exclusions", "", false);
    if (!exclusions)
        return exclusions.error();
    if (exclusions.value() != nullptr) {
        for (std::size_t index = 0; index < exclusions.value()->size(); ++index) {
            const std::string at = index_path("exclusions", index);
            const Json &value = (*exclusions.value())[index];
            if (!value.is_object())
                return invalid(at, "expected an object");
            const Object &entry = *value.as_object();
            if (auto keys = check_keys(entry, at, {"benchmark", "target", "reason"}); !keys)
                return keys.error();
            ExclusionSpec exclusion;
            auto benchmark = required_string(entry, "benchmark", at);
            if (!benchmark)
                return benchmark.error();
            exclusion.benchmark = benchmark.value();
            auto target = required_string(entry, "target", at);
            if (!target)
                return target.error();
            exclusion.target = target.value();
            auto reason = optional_string(entry, "reason", at, "");
            if (!reason)
                return reason.error();
            exclusion.reason = reason.value();
            if (benchmark_ids.find(exclusion.benchmark) == benchmark_ids.end())
                return invalid(join_path(at, "benchmark"),
                               "unknown benchmark '" + exclusion.benchmark + "'");
            if (target_ids.find(exclusion.target) == target_ids.end())
                return invalid(join_path(at, "target"), "unknown target '" + exclusion.target + "'");
            manifest.exclusions.push_back(std::move(exclusion));
        }
    }
    return manifest;
}

Result<Manifest> load_manifest(std::string_view path) {
    std::ifstream stream{std::string(path), std::ios::binary};
    if (!stream)
        return invalid(std::string(path), "cannot open manifest", ErrorCategory::NotFound);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof())
        return invalid(std::string(path), "cannot read manifest");
    return parse_manifest(buffer.str(), path, parent_directory(path));
}

Diagnostics validate_manifest(const Manifest &manifest) {
    Diagnostics diagnostics;
    auto report = [&diagnostics](std::string pointer, std::string message,
                                 Severity severity = Severity::Error) {
        Diagnostic diagnostic;
        diagnostic.id = "mahak.manifest";
        diagnostic.message = std::move(message);
        diagnostic.severity = severity;
        diagnostic.primary.file = pointer.empty() ? "<manifest>" : std::move(pointer);
        diagnostics.push_back(std::move(diagnostic));
    };

    if (manifest.schema != mahak_manifest_schema_version)
        report("schema", "unsupported manifest schema");
    if (manifest.id.empty())
        report("id", "manifest id must not be empty");

    std::set<std::string> target_ids;
    for (std::size_t t = 0; t < manifest.targets.size(); ++t) {
        const TargetSpec &target = manifest.targets[t];
        const std::string at = index_path("targets", t);
        if (!target_ids.insert(target.id).second)
            report(at + ".id", "duplicate target id '" + target.id + "'");
        StageKind previous = StageKind::Setup;
        bool first = true;
        for (std::size_t s = 0; s < target.stages.size(); ++s) {
            const StageSpec &stage = target.stages[s];
            const std::string stage_at = index_path(at + ".stages", s);
            if (!first && !stage_ordered(previous, stage.kind))
                report(stage_at + ".kind", "stages must follow Setup, Compile, Link, Validate, "
                                           "Execute, Teardown order");
            previous = stage.kind;
            first = false;
            if (stage.commands.empty())
                report(stage_at + ".commands", "stage has no commands");
        }
        for (const auto &parameter : target.parameters) {
            if (!parameter.values.is_array() || parameter.values.as_array()->empty())
                report(at + ".parameters", "parameter '" + parameter.name + "' has no values");
        }
        if (!target.execute && target.stages.empty())
            report(at, "target has neither stages nor an execute command");
    }

    std::set<std::string> benchmark_ids;
    for (std::size_t b = 0; b < manifest.benchmarks.size(); ++b) {
        const BenchmarkSpec &benchmark = manifest.benchmarks[b];
        const std::string at = index_path("benchmarks", b);
        if (!benchmark_ids.insert(benchmark.id).second)
            report(at + ".id", "duplicate benchmark id '" + benchmark.id + "'");
        if (benchmark.modes.empty())
            report(at + ".modes", "benchmark must declare at least one execution mode",
                   Severity::Warning);
        StageKind previous = StageKind::Setup;
        bool first = true;
        for (std::size_t s = 0; s < benchmark.stages.size(); ++s) {
            const StageSpec &stage = benchmark.stages[s];
            const std::string stage_at = index_path(at + ".stages", s);
            if (!first && !stage_ordered(previous, stage.kind))
                report(stage_at + ".kind", "stages must follow Setup, Compile, Link, Validate, "
                                           "Execute, Teardown order");
            previous = stage.kind;
            first = false;
            if (stage.commands.empty())
                report(stage_at + ".commands", "stage has no commands");
        }
        for (std::size_t p = 0; p < benchmark.parameters.size(); ++p) {
            const ParameterSpec &parameter = benchmark.parameters[p];
            if (!parameter.values.is_array() || parameter.values.as_array()->empty())
                report(index_path(at + ".parameters", p) + ".values", "parameter has no values");
        }
        // Duplicate parameter leaves across target and benchmark are ambiguous.
        std::set<std::string> leaves;
        for (const auto &target : manifest.targets)
            for (const auto &parameter : target.parameters)
                leaves.insert(parameter.name);
        for (const auto &parameter : benchmark.parameters) {
            if (leaves.count(parameter.name) != 0)
                report(at + ".parameters", "parameter '" + parameter.name +
                                               "' is declared by both a target and a benchmark");
        }
        if (benchmark.sampling.repetitions == 0)
            report(at + ".sampling.repetitions", "repetitions must be at least 1");
        // A benchmark may rely entirely on its target's stages; only report an
        // unrunnable benchmark when no target can supply an execute stage.
        if (!benchmark.execute && benchmark.stages.empty()) {
            bool target_can_execute = false;
            for (const auto &target : manifest.targets) {
                target_can_execute = target_can_execute || target.execute.has_value();
                for (const auto &stage : target.stages)
                    target_can_execute = target_can_execute || stage.kind == StageKind::Execute;
            }
            if (!target_can_execute)
                report(at, "benchmark has neither stages nor an execute command");
        }
        for (const auto &mode : benchmark.modes) {
            bool has_execute = benchmark.execute.has_value();
            for (const auto &stage : benchmark.stages)
                has_execute = has_execute || stage.kind == StageKind::Execute;
            for (const auto &target : manifest.targets) {
                bool target_has_execute = target.execute.has_value();
                for (const auto &stage : target.stages)
                    target_has_execute = target_has_execute || stage.kind == StageKind::Execute;
                if (mode == ExecutionMode::ExecuteOnly && !has_execute && !target_has_execute)
                    report(at + ".modes", "execute-only requires an execute command");
            }
        }
    }

    std::set<std::pair<std::string, std::string>> seen_exclusions;
    for (std::size_t e = 0; e < manifest.exclusions.size(); ++e) {
        const ExclusionSpec &exclusion = manifest.exclusions[e];
        const std::string at = index_path("exclusions", e);
        if (benchmark_ids.count(exclusion.benchmark) == 0)
            report(at + ".benchmark", "unknown benchmark '" + exclusion.benchmark + "'");
        if (target_ids.count(exclusion.target) == 0)
            report(at + ".target", "unknown target '" + exclusion.target + "'");
        if (!seen_exclusions.insert({exclusion.benchmark, exclusion.target}).second)
            report(at, "duplicate exclusion for the same benchmark and target");
    }

    std::sort(diagnostics.begin(), diagnostics.end(),
              [](const Diagnostic &left, const Diagnostic &right) {
                  if (left.primary.file != right.primary.file)
                      return left.primary.file < right.primary.file;
                  return left.message < right.message;
              });
    return diagnostics;
}

std::string format_diagnostics(const Diagnostics &diagnostics) {
    std::string out;
    for (const auto &diagnostic : diagnostics) {
        if (!out.empty())
            out.push_back('\n');
        out += name(diagnostic.severity);
        out += ": ";
        out += diagnostic.primary.file.empty() ? "<manifest>" : diagnostic.primary.file;
        out += ": ";
        out += diagnostic.message;
    }
    return out;
}

std::string manifest_digest(const Manifest &manifest) {
    const std::string text = digest_view(manifest).dump();
    std::uint64_t hash = 1469598103934665603ull; // FNV offset basis
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull; // FNV prime
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string hex(16, '0');
    for (int index = 15; index >= 0; --index) {
        hex[static_cast<std::size_t>(index)] = digits[hash & 0xF];
        hash >>= 4;
    }
    return hex;
}

Result<SuitePlan> plan_manifest(const Manifest &manifest, const PlanOptions &options) {
    if (!validate_manifest(manifest).empty()) {
        const Diagnostics diagnostics = validate_manifest(manifest);
        for (const auto &diagnostic : diagnostics) {
            if (diagnostic.severity == Severity::Error)
                return invalid(diagnostic.primary.file, diagnostic.message);
        }
    }
    SuitePlan plan;
    plan.manifest_id = manifest.id;
    plan.manifest_version = manifest.version;
    plan.base_directory = manifest.base_directory;
    plan.seed = options.seed != 0 ? options.seed : manifest.seed;

    const std::string work_root =
        options.work_root.empty() ? std::string() : options.work_root;

    std::vector<const BenchmarkSpec *> benchmarks;
    for (const auto &benchmark : manifest.benchmarks) {
        if (!options.benchmarks.empty() && options.benchmarks.count(benchmark.id) == 0)
            continue;
        if (!options.groups.empty() && options.groups.count(benchmark.group) == 0)
            continue;
        if (!options.tags.empty()) {
            bool matched = false;
            for (const auto &tag : benchmark.tags)
                matched = matched || options.tags.count(tag) != 0;
            if (!matched)
                continue;
        }
        benchmarks.push_back(&benchmark);
    }
    std::sort(benchmarks.begin(), benchmarks.end(),
              [](const BenchmarkSpec *left, const BenchmarkSpec *right) {
                  return left->id < right->id;
              });

    std::vector<const TargetSpec *> targets;
    for (const auto &target : manifest.targets) {
        if (!options.targets.empty() && options.targets.count(target.id) == 0)
            continue;
        targets.push_back(&target);
    }
    std::sort(targets.begin(), targets.end(),
              [](const TargetSpec *left, const TargetSpec *right) {
                  return left->id < right->id;
              });

    for (const BenchmarkSpec *benchmark : benchmarks) {
        std::set<ExecutionMode> modes = options.modes;
        if (modes.empty())
            modes.insert(benchmark->modes.begin(), benchmark->modes.end());
        if (modes.empty())
            modes.insert(ExecutionMode::EndToEnd);

        for (const TargetSpec *target : targets) {
            bool excluded = false;
            for (const auto &exclusion : manifest.exclusions) {
                if (exclusion.benchmark == benchmark->id && exclusion.target == target->id) {
                    excluded = true;
                    break;
                }
            }
            if (excluded && !options.include_excluded)
                continue;

            // Parameter matrix: target parameters first, then benchmark
            // parameters, in declaration order.
            std::vector<ParameterSpec> matrix = target->parameters;
            matrix.insert(matrix.end(), benchmark->parameters.begin(),
                          benchmark->parameters.end());
            std::vector<std::pair<std::string, std::string>> pinned;
            for (const auto &override_entry : options.parameter_overrides) {
                bool known = false;
                for (const auto &parameter : matrix)
                    known = known || parameter.name == override_entry.first;
                if (!known)
                    return invalid("parameters",
                                   "override for unknown parameter '" + override_entry.first + "'");
                pinned.emplace_back(override_entry.first, override_entry.second);
            }
            std::vector<std::vector<ResolvedParameter>> combinations(1);
            for (const auto &parameter : matrix) {
                std::vector<std::vector<ResolvedParameter>> next;
                const Json::Array *raw_values = parameter.values.as_array();
                if (raw_values == nullptr)
                    return invalid("parameters", "parameter '" + parameter.name + "' is not an array");
                std::vector<std::string> values;
                for (const auto &value : *raw_values)
                    values.push_back(format_parameter_value(value));
                if (values.empty())
                    return invalid("parameters", "parameter '" + parameter.name + "' has no values");
                for (const auto &prefix : combinations) {
                    for (std::size_t index = 0; index < raw_values->size(); ++index) {
                        const std::string text = values[index];
                        bool skip = false;
                        for (const auto &pin : pinned) {
                            if (pin.first == parameter.name && pin.second != text)
                                skip = true;
                        }
                        if (skip)
                            continue;
                        std::vector<ResolvedParameter> combination = prefix;
                        combination.push_back(
                            ResolvedParameter{parameter.name, (*raw_values)[index], text});
                        next.push_back(std::move(combination));
                    }
                }
                combinations = std::move(next);
            }
            if (combinations.size() == 1 && combinations.front().empty() && !pinned.empty())
                return invalid("parameters", "overrides removed every parameter combination");

            for (const auto &combination : combinations) {
                CasePlan case_plan;
                case_plan.benchmark_id = benchmark->id;
                case_plan.target_id = target->id;
                case_plan.group = benchmark->group;
                case_plan.description = benchmark->description;
                case_plan.tags = benchmark->tags;
                case_plan.modes.assign(modes.begin(), modes.end());
                case_plan.parameters = combination;
                case_plan.oracle = benchmark->oracle;
                case_plan.metrics = benchmark->metrics;
                case_plan.sampling = benchmark->sampling;
                case_plan.seed = plan.seed != 0 ? plan.seed : case_plan.sampling.seed;

                std::string suffix;
                for (const auto &parameter : combination) {
                    if (!suffix.empty())
                        suffix.push_back('-');
                    suffix += sanitize_component(parameter.name) + "=" +
                              sanitize_component(parameter.text);
                }
                case_plan.id = sanitize_component(benchmark->id) + "@" +
                               sanitize_component(target->id) +
                               (suffix.empty() ? std::string() : "-" + suffix);
                const std::string work_name = sanitize_component(case_plan.id);
                case_plan.work_directory =
                    work_root.empty() ? work_name : absolute_join(work_root, work_name);

                std::map<std::string, std::string> variables;
                variables["benchmark"] = benchmark->id;
                variables["target"] = target->id;
                variables["case"] = case_plan.id;
                variables["work"] = case_plan.work_directory;
                for (const auto &parameter : combination)
                    variables["param:" + parameter.name] = parameter.text;
                for (const auto &fixture : benchmark->fixtures) {
                    case_plan.fixtures[fixture.target_name] =
                        absolute_join(manifest.base_directory, fixture.path);
                    // Fixtures are copied into <work>/fixtures/<target name>;
                    // {fixture:NAME} uses the declared fixture name and also
                    // accepts the copied file name as an alias.
                    const std::string copied = "fixtures/" + fixture.target_name;
                    variables["fixture:" + fixture.name] = copied;
                    variables["fixture:" + fixture.target_name] = copied;
                }
                // Artifact names are collected from every command so they can be
                // referenced from any stage, then resolved inside the work dir.
                std::vector<const CommandSpec *> commands;
                for (const auto &stage : target->stages)
                    for (const auto &command : stage.commands)
                        commands.push_back(&command);
                for (const auto &stage : benchmark->stages)
                    for (const auto &command : stage.commands)
                        commands.push_back(&command);
                if (target->execute)
                    commands.push_back(&*target->execute);
                if (benchmark->execute)
                    commands.push_back(&*benchmark->execute);
                for (const CommandSpec *command : commands) {
                    for (const auto &artifact : command->artifacts) {
                        if (case_plan.artifacts.count(artifact) != 0)
                            continue;
                        case_plan.artifacts[artifact] = artifact;
                        // Artifacts live in the case work directory and are not
                        // searched through PATH; resolve them explicitly.
                        variables["artifact:" + artifact] = "./" + artifact;
                    }
                }
                if (!benchmark->fixtures.empty()) {
                    variables["source"] =
                        "fixtures/" + benchmark->fixtures.front().target_name;
                }
                if (!case_plan.artifacts.empty())
                    variables["output"] = "./" + case_plan.artifacts.begin()->second;

                auto expand = [&](const std::string &input, const std::string &at) -> Result<std::string> {
                    bool unresolved = false;
                    std::string out = expand_placeholders(input, variables, &unresolved);
                    if (unresolved)
                        return invalid(at, "unresolved placeholder in '" + input + "'");
                    if (out.find('{') != std::string::npos)
                        return invalid(at, "unresolved placeholder in '" + input + "'");
                    return out;
                };

                auto append_stages = [&](const std::vector<StageSpec> &stages,
                                         const std::string &owner) -> Result<void> {
                    for (const auto &stage : stages) {
                        bool needed = false;
                        for (const auto &mode : modes)
                            needed = needed || mode_needs(mode, stage.kind);
                        if (!needed)
                            continue;
                        StagePlan stage_plan;
                        stage_plan.kind = stage.kind;
                        stage_plan.optional = stage.optional;
                        for (const auto &command : stage.commands) {
                            CommandPlan planned;
                            for (const auto &argument : command.argv) {
                                auto value = expand(argument, owner + ".argv");
                                if (!value)
                                    return value.error();
                                planned.argv.push_back(value.value());
                            }
                            planned.working_directory = case_plan.work_directory;
                            planned.environment = target->environment;
                            planned.environment.insert(planned.environment.end(),
                                                       command.environment.begin(),
                                                       command.environment.end());
                            if (command.stdin_data && !command.stdin_data->empty()) {
                                auto value = expand(*command.stdin_data, owner + ".stdin_data");
                                if (!value)
                                    return value.error();
                                planned.stdin_data = value.value();
                            }
                            planned.timeout_ms = command.timeout_ms;
                            stage_plan.commands.push_back(std::move(planned));
                        }
                        case_plan.stages.push_back(std::move(stage_plan));
                    }
                    return {};
                };

                if (auto built = append_stages(target->stages, case_plan.id + ".target"); !built)
                    return built.error();
                if (auto built = append_stages(benchmark->stages, case_plan.id + ".benchmark");
                    !built)
                    return built.error();

                const CommandSpec *execute_command = nullptr;
                if (benchmark->execute)
                    execute_command = &*benchmark->execute;
                else if (target->execute)
                    execute_command = &*target->execute;
                if (execute_command != nullptr) {
                    bool needed = false;
                    for (const auto &mode : modes)
                        needed = needed || mode_needs(mode, StageKind::Execute);
                    if (needed) {
                        CommandPlan planned;
                        for (const auto &argument : execute_command->argv) {
                            auto value = expand(argument, case_plan.id + ".execute.argv");
                            if (!value)
                                return value.error();
                            planned.argv.push_back(value.value());
                        }
                        planned.working_directory = case_plan.work_directory;
                        planned.environment = target->environment;
                        planned.environment.insert(planned.environment.end(),
                                                   execute_command->environment.begin(),
                                                   execute_command->environment.end());
                        if (execute_command->stdin_data && !execute_command->stdin_data->empty()) {
                            auto value = expand(*execute_command->stdin_data,
                                                case_plan.id + ".execute.stdin_data");
                            if (!value)
                                return value.error();
                            planned.stdin_data = value.value();
                        }
                        planned.timeout_ms = execute_command->timeout_ms;
                        StagePlan execute_stage;
                        execute_stage.kind = StageKind::Execute;
                        execute_stage.commands.push_back(std::move(planned));
                        case_plan.stages.push_back(std::move(execute_stage));
                    }
                }

                if (options.require_runnable) {
                    bool runnable = false;
                    for (const auto &stage : case_plan.stages)
                        runnable = runnable || stage.kind == StageKind::Execute;
                    if (!runnable)
                        return invalid(case_plan.id, "case has no runnable execute stage");
                }
                plan.cases.push_back(std::move(case_plan));
            }
        }
    }
    if (plan.cases.empty())
        return invalid("plan", "filters selected no benchmark cases");
    return plan;
}

} // namespace v1
} // namespace moonstone::mahak
