#include "mahak/Storage.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace moonstone::mahak {
inline namespace v1 {
namespace {

namespace fs = std::filesystem;

using Object = Json::Object;
using Array = Json::Array;

Validity parse_validity(std::string_view text, Validity fallback) {
    if (text == "valid")
        return Validity::Valid;
    if (text == "invalid-output")
        return Validity::InvalidOutput;
    if (text == "non-zero-exit")
        return Validity::NonZeroExit;
    if (text == "timed-out")
        return Validity::TimedOut;
    if (text == "crashed")
        return Validity::Crashed;
    if (text == "cancelled")
        return Validity::Cancelled;
    if (text == "unsupported")
        return Validity::Unsupported;
    if (text == "setup-failed")
        return Validity::SetupFailed;
    if (text == "measurement-failed")
        return Validity::MeasurementFailed;
    return fallback;
}

Error storage_error(std::string message, ErrorCategory category = ErrorCategory::InvalidInput) {
    Error error;
    error.category = category;
    error.message = std::move(message);
    return error;
}

Json encode_cpu(const CpuTimes &cpu) {
    Json value = Json::object();
    value.set("user_ns", Json(static_cast<std::int64_t>(cpu.user_ns)));
    value.set("system_ns", Json(static_cast<std::int64_t>(cpu.system_ns)));
    return value;
}

CpuTimes decode_cpu(const Json *value) {
    CpuTimes cpu;
    if (value == nullptr || !value->is_object())
        return cpu;
    if (const Json *item = value->find("user_ns")) {
        if (const auto number = item->as_uint64())
            cpu.user_ns = *number;
    }
    if (const Json *item = value->find("system_ns")) {
        if (const auto number = item->as_uint64())
            cpu.system_ns = *number;
    }
    return cpu;
}

Json encode_usage(const ResourceUsage &usage) {
    Json value = Json::object();
    value.set("max_rss_bytes", Json(static_cast<std::int64_t>(usage.max_rss_bytes)));
    value.set("minor_faults", Json(static_cast<std::int64_t>(usage.minor_faults)));
    value.set("major_faults", Json(static_cast<std::int64_t>(usage.major_faults)));
    value.set("voluntary_context_switches",
              Json(static_cast<std::int64_t>(usage.voluntary_context_switches)));
    value.set("involuntary_context_switches",
              Json(static_cast<std::int64_t>(usage.involuntary_context_switches)));
    value.set("read_bytes", Json(static_cast<std::int64_t>(usage.read_bytes)));
    value.set("write_bytes", Json(static_cast<std::int64_t>(usage.write_bytes)));
    Json notes = Json::array();
    for (const auto &note : usage.usage_notes)
        notes.push(Json(note));
    value.set("notes", std::move(notes));
    return value;
}

ResourceUsage decode_usage(const Json *value) {
    ResourceUsage usage;
    if (value == nullptr || !value->is_object())
        return usage;
    auto assign = [value](std::string_view key, std::uint64_t &slot) {
        if (const Json *item = value->find(key)) {
            if (const auto number = item->as_uint64())
                slot = *number;
        }
    };
    assign("max_rss_bytes", usage.max_rss_bytes);
    assign("minor_faults", usage.minor_faults);
    assign("major_faults", usage.major_faults);
    assign("voluntary_context_switches", usage.voluntary_context_switches);
    assign("involuntary_context_switches", usage.involuntary_context_switches);
    assign("read_bytes", usage.read_bytes);
    assign("write_bytes", usage.write_bytes);
    if (const Json *notes = value->find("notes")) {
        if (const auto *array = notes->as_array()) {
            for (const auto &note : *array) {
                if (const auto *text = note.as_string())
                    usage.usage_notes.push_back(*text);
            }
        }
    }
    return usage;
}

Json encode_perf(const std::vector<PerfCounter> &counters) {
    Json array = Json::array();
    for (const auto &counter : counters) {
        Json value = Json::object();
        value.set("name", Json(counter.name));
        value.set("value", Json(static_cast<std::int64_t>(counter.value)));
        value.set("enabled", Json(counter.enabled));
        value.set("scaled", Json(counter.scaled));
        value.set("scale", Json(counter.scale));
        value.set("note", Json(counter.note));
        array.push(std::move(value));
    }
    return array;
}

std::vector<PerfCounter> decode_perf(const Json *value) {
    std::vector<PerfCounter> counters;
    if (value == nullptr)
        return counters;
    const auto *array = value->as_array();
    if (array == nullptr)
        return counters;
    for (const auto &item : *array) {
        if (!item.is_object())
            continue;
        PerfCounter counter;
        if (const Json *name = item.find("name")) {
            if (const auto *text = name->as_string())
                counter.name = *text;
        }
        if (const Json *value_item = item.find("value")) {
            if (const auto number = value_item->as_uint64())
                counter.value = *number;
        }
        if (const Json *enabled = item.find("enabled")) {
            if (const auto flag = enabled->as_bool())
                counter.enabled = *flag;
        }
        if (const Json *scaled = item.find("scaled")) {
            if (const auto flag = scaled->as_bool())
                counter.scaled = *flag;
        }
        if (const Json *scale = item.find("scale")) {
            if (const auto number = scale->as_double())
                counter.scale = *number;
        }
        if (const Json *note = item.find("note")) {
            if (const auto *text = note->as_string())
                counter.note = *text;
        }
        counters.push_back(std::move(counter));
    }
    return counters;
}

Json encode_phases(const std::vector<PhaseEvent> &phases) {
    Json array = Json::array();
    for (const auto &phase : phases) {
        Json value = Json::object();
        value.set("name", Json(phase.name));
        value.set("category", Json(phase.category));
        value.set("start_ns", Json(static_cast<std::int64_t>(phase.start_ns)));
        value.set("duration_ns", Json(static_cast<std::int64_t>(phase.duration_ns)));
        value.set("value", Json(static_cast<std::int64_t>(phase.value)));
        value.set("detail", Json(phase.detail));
        array.push(std::move(value));
    }
    return array;
}

std::vector<PhaseEvent> decode_phases(const Json *value) {
    std::vector<PhaseEvent> phases;
    const auto *array = value != nullptr ? value->as_array() : nullptr;
    if (array == nullptr)
        return phases;
    for (const auto &item : *array) {
        if (!item.is_object())
            continue;
        PhaseEvent phase;
        auto text = [&item](std::string_view key, std::string &slot) {
            if (const Json *field = item.find(key)) {
                if (const auto *value = field->as_string())
                    slot = *value;
            }
        };
        auto number = [&item](std::string_view key, std::uint64_t &slot) {
            if (const Json *field = item.find(key)) {
                if (const auto value = field->as_uint64())
                    slot = *value;
            }
        };
        text("name", phase.name);
        text("category", phase.category);
        text("detail", phase.detail);
        number("start_ns", phase.start_ns);
        number("duration_ns", phase.duration_ns);
        phases.push_back(std::move(phase));
    }
    return phases;
}

Json encode_runtime(const std::vector<RuntimeMetric> &metrics) {
    Json array = Json::array();
    for (const auto &metric : metrics) {
        Json value = Json::object();
        value.set("name", Json(metric.name));
        value.set("unit", Json(metric.unit));
        value.set("value", Json(metric.value));
        value.set("source", Json(metric.source));
        array.push(std::move(value));
    }
    return array;
}

std::vector<RuntimeMetric> decode_runtime(const Json *value) {
    std::vector<RuntimeMetric> metrics;
    const auto *array = value != nullptr ? value->as_array() : nullptr;
    if (array == nullptr)
        return metrics;
    for (const auto &item : *array) {
        if (!item.is_object())
            continue;
        RuntimeMetric metric;
        if (const Json *name = item.find("name")) {
            if (const auto *text = name->as_string())
                metric.name = *text;
        }
        if (const Json *unit = item.find("unit")) {
            if (const auto *text = unit->as_string())
                metric.unit = *text;
        }
        if (const Json *value_item = item.find("value")) {
            if (const auto number = value_item->as_double())
                metric.value = *number;
        }
        if (const Json *source = item.find("source")) {
            if (const auto *text = source->as_string())
                metric.source = *text;
        }
        metrics.push_back(std::move(metric));
    }
    return metrics;
}

Json encode_snapshot(const MeasurementSnapshot &snapshot) {
    Json value = Json::object();
    value.set("wall_ns", Json(static_cast<std::int64_t>(snapshot.wall_ns)));
    value.set("cpu", encode_cpu(snapshot.cpu));
    value.set("usage", encode_usage(snapshot.usage));
    value.set("perf", encode_perf(snapshot.perf));
    value.set("phases", encode_phases(snapshot.phases));
    value.set("runtime", encode_runtime(snapshot.runtime));
    value.set("clock", Json(snapshot.clock));
    return value;
}

MeasurementSnapshot decode_snapshot(const Json *value) {
    MeasurementSnapshot snapshot;
    if (value == nullptr || !value->is_object())
        return snapshot;
    if (const Json *wall = value->find("wall_ns")) {
        if (const auto number = wall->as_uint64())
            snapshot.wall_ns = *number;
    }
    snapshot.cpu = decode_cpu(value->find("cpu"));
    snapshot.usage = decode_usage(value->find("usage"));
    snapshot.perf = decode_perf(value->find("perf"));
    snapshot.phases = decode_phases(value->find("phases"));
    snapshot.runtime = decode_runtime(value->find("runtime"));
    if (const Json *clock = value->find("clock")) {
        if (const auto *text = clock->as_string())
            snapshot.clock = *text;
    }
    return snapshot;
}

Json encode_command(const CommandResult &command) {
    Json value = Json::object();
    Json argv = Json::array();
    for (const auto &argument : command.argv)
        argv.push(Json(argument));
    value.set("argv", std::move(argv));
    value.set("exit_code", Json(static_cast<std::int64_t>(command.exit_code)));
    value.set("signal", Json(static_cast<std::int64_t>(command.signal)));
    value.set("timed_out", Json(command.timed_out));
    value.set("spawn_failed", Json(command.spawn_failed));
    value.set("output_truncated", Json(command.output_truncated));
    value.set("stdout", Json(command.stdout_text));
    value.set("stderr", Json(command.stderr_text));
    value.set("wall_ns", Json(static_cast<std::int64_t>(command.wall_ns)));
    value.set("cpu", encode_cpu(command.cpu));
    value.set("usage", encode_usage(command.usage));
    value.set("perf", encode_perf(command.perf));
    value.set("validity", Json(name(command.validity)));
    value.set("message", Json(command.message));
    return value;
}

CommandResult decode_command(const Json &value) {
    CommandResult command;
    if (!value.is_object())
        return command;
    if (const Json *argv = value.find("argv")) {
        if (const auto *array = argv->as_array()) {
            for (const auto &argument : *array) {
                if (const auto *text = argument.as_string())
                    command.argv.push_back(*text);
            }
        }
    }
    auto integer = [&value](std::string_view key, int fallback) {
        const Json *field = value.find(key);
        const auto number = field != nullptr ? field->as_int64() : std::nullopt;
        return number ? static_cast<int>(*number) : fallback;
    };
    command.exit_code = integer("exit_code", -1);
    command.signal = integer("signal", 0);
    auto boolean = [&value](std::string_view key) {
        const Json *field = value.find(key);
        const auto flag = field != nullptr ? field->as_bool() : std::nullopt;
        return flag ? *flag : false;
    };
    command.timed_out = boolean("timed_out");
    command.spawn_failed = boolean("spawn_failed");
    command.output_truncated = boolean("output_truncated");
    auto text = [&value](std::string_view key, std::string fallback) {
        const Json *field = value.find(key);
        const std::string *string = field != nullptr ? field->as_string() : nullptr;
        return string != nullptr ? *string : fallback;
    };
    command.stdout_text = text("stdout", {});
    command.stderr_text = text("stderr", {});
    const Json *wall = value.find("wall_ns");
    const auto wall_value = wall != nullptr ? wall->as_uint64() : std::nullopt;
    command.wall_ns = wall_value ? *wall_value : 0;
    command.cpu = decode_cpu(value.find("cpu"));
    command.usage = decode_usage(value.find("usage"));
    command.perf = decode_perf(value.find("perf"));
    command.message = text("message", {});
    const std::string validity_text = text("validity", "valid");
    command.validity = Validity::Valid;
    if (validity_text == "invalid-output")
        command.validity = Validity::InvalidOutput;
    else if (validity_text == "non-zero-exit")
        command.validity = Validity::NonZeroExit;
    else if (validity_text == "timed-out")
        command.validity = Validity::TimedOut;
    else if (validity_text == "crashed")
        command.validity = Validity::Crashed;
    else if (validity_text == "cancelled")
        command.validity = Validity::Cancelled;
    else if (validity_text == "unsupported")
        command.validity = Validity::Unsupported;
    else if (validity_text == "setup-failed")
        command.validity = Validity::SetupFailed;
    else if (validity_text == "measurement-failed")
        command.validity = Validity::MeasurementFailed;
    return command;
}

Json encode_stage(const StageResult &stage) {
    Json value = Json::object();
    value.set("kind", Json(name(stage.kind)));
    Json commands = Json::array();
    for (const auto &command : stage.commands)
        commands.push(encode_command(command));
    value.set("commands", std::move(commands));
    return value;
}

StageResult decode_stage(const Json &value) {
    StageResult stage;
    if (!value.is_object())
        return stage;
    if (const Json *kind = value.find("kind")) {
        if (const auto *text = kind->as_string()) {
            if (const auto parsed = parse_stage_kind(*text))
                stage.kind = *parsed;
        }
    }
    if (const Json *commands = value.find("commands")) {
        if (const auto *array = commands->as_array()) {
            for (const auto &command : *array)
                stage.commands.push_back(decode_command(command));
        }
    }
    return stage;
}

Json encode_oracle(const OracleResult &oracle) {
    Json value = Json::object();
    value.set("passed", Json(oracle.passed));
    value.set("validity", Json(name(oracle.validity)));
    value.set("message", Json(oracle.message));
    return value;
}

OracleResult decode_oracle(const Json *value) {
    OracleResult oracle;
    if (value == nullptr || !value->is_object())
        return oracle;
    if (const Json *passed = value->find("passed")) {
        if (const auto flag = passed->as_bool())
            oracle.passed = *flag;
    }
    if (const Json *validity = value->find("validity")) {
        if (const auto *text = validity->as_string()) {
            if (*text == "valid")
                oracle.validity = Validity::Valid;
            else if (*text == "invalid-output")
                oracle.validity = Validity::InvalidOutput;
            else if (*text == "non-zero-exit")
                oracle.validity = Validity::NonZeroExit;
            else if (*text == "timed-out")
                oracle.validity = Validity::TimedOut;
            else if (*text == "crashed")
                oracle.validity = Validity::Crashed;
            else if (*text == "cancelled")
                oracle.validity = Validity::Cancelled;
            else if (*text == "unsupported")
                oracle.validity = Validity::Unsupported;
            else if (*text == "setup-failed")
                oracle.validity = Validity::SetupFailed;
            else if (*text == "measurement-failed")
                oracle.validity = Validity::MeasurementFailed;
        }
    }
    if (const Json *message = value->find("message")) {
        if (const auto *text = message->as_string())
            oracle.message = *text;
    }
    return oracle;
}

Json encode_observation(const Observation &observation) {
    Json value = Json::object();
    value.set("metric", Json(observation.metric));
    value.set("unit", Json(observation.unit));
    value.set("direction", Json(name(observation.direction)));
    value.set("value", Json(observation.value));
    value.set("sequence", Json(static_cast<std::int64_t>(observation.sequence)));
    value.set("repetition", Json(static_cast<std::int64_t>(observation.repetition)));
    value.set("valid", Json(observation.valid));
    value.set("validity", Json(name(observation.validity)));
    value.set("provenance", Json(observation.provenance));
    return value;
}

Observation decode_observation(const Json *value) {
    Observation observation;
    if (value == nullptr || !value->is_object())
        return observation;
    if (const Json *metric = value->find("metric")) {
        if (const auto *text = metric->as_string())
            observation.metric = *text;
    }
    if (const Json *unit = value->find("unit")) {
        if (const auto *text = unit->as_string())
            observation.unit = *text;
    }
    if (const Json *direction = value->find("direction")) {
        if (const auto *text = direction->as_string()) {
            if (const auto parsed = parse_metric_direction(*text))
                observation.direction = *parsed;
        }
    }
    if (const Json *item = value->find("value")) {
        if (const auto number = item->as_double())
            observation.value = *number;
    }
    if (const Json *item = value->find("sequence")) {
        if (const auto number = item->as_uint64())
            observation.sequence = *number;
    }
    if (const Json *item = value->find("repetition")) {
        if (const auto number = item->as_uint64())
            observation.repetition = *number;
    }
    if (const Json *item = value->find("valid")) {
        if (const auto flag = item->as_bool())
            observation.valid = *flag;
    }
    if (const Json *item = value->find("provenance")) {
        if (const auto *text = item->as_string())
            observation.provenance = *text;
    }
    if (const Json *item = value->find("validity")) {
        if (const auto *text = item->as_string())
            observation.validity = parse_validity(*text, Validity::MeasurementFailed);
    }
    return observation;
}

Json encode_sample(const SampleRecord &sample) {
    Json value = Json::object();
    value.set("index", Json(static_cast<std::int64_t>(sample.index)));
    value.set("repetition", Json(static_cast<std::int64_t>(sample.repetition)));
    value.set("configuration", Json(sample.configuration));
    value.set("started_unix_ns", Json(static_cast<std::int64_t>(sample.started_unix_ns)));
    value.set("observation", encode_observation(sample.observation));
    value.set("measurement", encode_snapshot(sample.measurement));
    return value;
}

SampleRecord decode_sample(const Json &value) {
    SampleRecord sample;
    if (!value.is_object())
        return sample;
    if (const Json *index = value.find("index")) {
        if (const auto number = index->as_uint64())
            sample.index = *number;
    }
    if (const Json *repetition = value.find("repetition")) {
        if (const auto number = repetition->as_uint64())
            sample.repetition = *number;
    }
    if (const Json *configuration = value.find("configuration")) {
        if (const auto *text = configuration->as_string())
            sample.configuration = *text;
    }
    if (const Json *started = value.find("started_unix_ns")) {
        if (const auto number = started->as_uint64())
            sample.started_unix_ns = *number;
    }
    sample.observation = decode_observation(value.find("observation"));
    sample.measurement = decode_snapshot(value.find("measurement"));
    return sample;
}

Json encode_run(const RunRecord &run) {
    Json value = Json::object();
    value.set("case_id", Json(run.case_id));
    value.set("benchmark_id", Json(run.benchmark_id));
    value.set("target_id", Json(run.target_id));
    value.set("configuration", Json(run.configuration));
    Json parameters = Json::array();
    for (const auto &parameter : run.parameters) {
        Json entry = Json::object();
        entry.set("name", Json(parameter.name));
        entry.set("value", parameter.value);
        entry.set("text", Json(parameter.text));
        parameters.push(std::move(entry));
    }
    value.set("parameters", std::move(parameters));
    value.set("status", Json(run.status));
    value.set("started_unix_ns", Json(static_cast<std::int64_t>(run.started_unix_ns)));
    value.set("duration_ns", Json(static_cast<std::int64_t>(run.duration_ns)));
    value.set("oracle", encode_oracle(run.oracle));
    value.set("message", Json(run.message));
    Json stages = Json::array();
    for (const auto &stage : run.stages)
        stages.push(encode_stage(stage));
    value.set("stages", std::move(stages));
    Json samples = Json::array();
    for (const auto &sample : run.samples)
        samples.push(encode_sample(sample));
    value.set("samples", std::move(samples));
    return value;
}

RunRecord decode_run(const Json &value) {
    RunRecord run;
    if (!value.is_object())
        return run;
    auto text = [&value](std::string_view key, std::string &slot) {
        if (const Json *field = value.find(key)) {
            if (const auto *string = field->as_string())
                slot = *string;
        }
    };
    auto number = [&value](std::string_view key, std::uint64_t &slot) {
        if (const Json *field = value.find(key)) {
            if (const auto integer = field->as_uint64())
                slot = *integer;
        }
    };
    text("case_id", run.case_id);
    text("benchmark_id", run.benchmark_id);
    text("target_id", run.target_id);
    text("configuration", run.configuration);
    text("status", run.status);
    text("message", run.message);
    number("started_unix_ns", run.started_unix_ns);
    number("duration_ns", run.duration_ns);
    run.oracle = decode_oracle(value.find("oracle"));
    if (const Json *parameters = value.find("parameters")) {
        if (const auto *array = parameters->as_array()) {
            for (const auto &item : *array) {
                if (!item.is_object())
                    continue;
                ResolvedParameter parameter;
                if (const Json *name = item.find("name")) {
                    if (const auto *string = name->as_string())
                        parameter.name = *string;
                }
                if (const Json *text_value = item.find("text")) {
                    if (const auto *string = text_value->as_string())
                        parameter.text = *string;
                }
                if (const Json *json_value = item.find("value"))
                    parameter.value = *json_value;
                run.parameters.push_back(std::move(parameter));
            }
        }
    }
    if (const Json *stages = value.find("stages")) {
        if (const auto *array = stages->as_array()) {
            for (const auto &item : *array)
                run.stages.push_back(decode_stage(item));
        }
    }
    if (const Json *samples = value.find("samples")) {
        if (const auto *array = samples->as_array()) {
            for (const auto &item : *array)
                run.samples.push_back(decode_sample(item));
        }
    }
    return run;
}

Json encode_environment(const EnvironmentInfo &environment) {
    Json value = Json::object();
    value.set("os", Json(environment.os));
    value.set("kernel", Json(environment.kernel));
    value.set("architecture", Json(environment.architecture));
    value.set("cpu_model", Json(environment.cpu_model));
    value.set("logical_cpus", Json(static_cast<std::int64_t>(environment.logical_cpus)));
    value.set("physical_cpus", Json(static_cast<std::int64_t>(environment.physical_cpus)));
    value.set("cpu_frequency_policy", Json(environment.cpu_frequency_policy));
    value.set("affinity", Json(environment.affinity));
    value.set("compiler", Json(environment.compiler));
    value.set("compiler_version", Json(environment.compiler_version));
    value.set("toolchain", Json(environment.toolchain));
    Json extra = Json::object();
    for (const auto &entry : environment.extra)
        extra.set(entry.first, Json(entry.second));
    value.set("extra", std::move(extra));
    return value;
}

EnvironmentInfo decode_environment(const Json *value) {
    EnvironmentInfo environment;
    if (value == nullptr || !value->is_object())
        return environment;
    auto text = [value](std::string_view key, std::string &slot) {
        if (const Json *field = value->find(key)) {
            if (const auto *string = field->as_string())
                slot = *string;
        }
    };
    text("os", environment.os);
    text("kernel", environment.kernel);
    text("architecture", environment.architecture);
    text("cpu_model", environment.cpu_model);
    text("cpu_frequency_policy", environment.cpu_frequency_policy);
    text("affinity", environment.affinity);
    text("compiler", environment.compiler);
    text("compiler_version", environment.compiler_version);
    text("toolchain", environment.toolchain);
    if (const Json *cpus = value->find("logical_cpus")) {
        if (const auto number = cpus->as_uint64())
            environment.logical_cpus = static_cast<std::uint32_t>(*number);
    }
    if (const Json *cpus = value->find("physical_cpus")) {
        if (const auto number = cpus->as_uint64())
            environment.physical_cpus = static_cast<std::uint32_t>(*number);
    }
    if (const Json *extra = value->find("extra")) {
        if (const auto *object = extra->as_object()) {
            for (const auto &entry : *object) {
                if (const auto *string = entry.second.as_string())
                    environment.extra[entry.first] = *string;
            }
        }
    }
    return environment;
}

std::string csv_escape(std::string_view value) {
    const bool needs_quotes =
        value.find_first_of(",\"\n\r") != std::string_view::npos;
    if (!needs_quotes)
        return std::string(value);
    std::string escaped = "\"";
    for (char character : value) {
        if (character == '"')
            escaped.push_back('"');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

bool safe_session_id(std::string_view id) {
    if (id.empty())
        return false;
    for (char character : id) {
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '-' ||
                          character == '_' || character == '.';
        if (!safe)
            return false;
    }
    return id != "." && id != ".." && id.find("..") == std::string_view::npos;
}

bool write_file_atomic(const fs::path &path, const std::string &contents) {
    const fs::path temporary = path.string() + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    std::size_t offset = 0;
    bool ok = true;
    while (offset < contents.size()) {
        const ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (written <= 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (ok && ::fsync(fd) != 0)
        ok = false;
    ::close(fd);
    if (!ok) {
        ::unlink(temporary.c_str());
        return false;
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) {
        ::fsync(directory);
        ::close(directory);
    }
    return true;
}

} // namespace

Result<Json> encode_session(const SessionRecord &session) {
    Json value = Json::object();
    value.set("schema", Json(static_cast<std::int64_t>(session.schema)));
    value.set("id", Json(session.id));
    value.set("manifest_id", Json(session.manifest_id));
    value.set("manifest_version", Json(session.manifest_version));
    value.set("manifest_digest", Json(session.manifest_digest));
    value.set("created_unix_ns", Json(static_cast<std::int64_t>(session.created_unix_ns)));
    value.set("finished_unix_ns", Json(static_cast<std::int64_t>(session.finished_unix_ns)));
    value.set("status", Json(session.status));
    value.set("host", Json(session.host));
    value.set("environment", encode_environment(session.environment));
    Json runs = Json::array();
    for (const auto &run : session.runs)
        runs.push(encode_run(run));
    value.set("runs", std::move(runs));
    Json notes = Json::array();
    for (const auto &note : session.notes)
        notes.push(Json(note));
    value.set("notes", std::move(notes));
    return value;
}

Result<SessionRecord> decode_session(const Json &json) {
    if (!json.is_object())
        return storage_error("session document must be a JSON object");
    const Json *schema = json.find("schema");
    const auto schema_value = schema != nullptr ? schema->as_uint64() : std::nullopt;
    if (!schema_value)
        return storage_error("session document is missing an integer 'schema'");
    if (*schema_value > mahak_result_schema_version)
        return storage_error("unsupported result schema " + std::to_string(*schema_value) +
                                 "; this build understands up to " +
                                 std::to_string(mahak_result_schema_version),
                             ErrorCategory::Unsupported);
    SessionRecord session;
    session.schema = static_cast<std::uint32_t>(*schema_value);
    auto text = [&json](std::string_view key, std::string &slot) {
        const Json *field = json.find(key);
        const std::string *string = field != nullptr ? field->as_string() : nullptr;
        if (string != nullptr)
            slot = *string;
    };
    auto number = [&json](std::string_view key, std::uint64_t &slot) {
        const Json *field = json.find(key);
        const auto integer = field != nullptr ? field->as_uint64() : std::nullopt;
        if (integer)
            slot = *integer;
    };
    text("id", session.id);
    text("manifest_id", session.manifest_id);
    text("manifest_version", session.manifest_version);
    text("manifest_digest", session.manifest_digest);
    text("status", session.status);
    text("host", session.host);
    number("created_unix_ns", session.created_unix_ns);
    number("finished_unix_ns", session.finished_unix_ns);
    session.environment = decode_environment(json.find("environment"));
    if (const Json *runs = json.find("runs")) {
        const auto *array = runs->as_array();
        if (array == nullptr)
            return storage_error("'runs' must be an array");
        for (const auto &run : *array)
            session.runs.push_back(decode_run(run));
    }
    if (const Json *notes = json.find("notes")) {
        const auto *array = notes->as_array();
        if (array == nullptr)
            return storage_error("'notes' must be an array");
        for (const auto &note : *array) {
            const auto *string = note.as_string();
            if (string == nullptr)
                return storage_error("'notes' entries must be strings");
            session.notes.push_back(*string);
        }
    }
    return session;
}

Result<std::string> encode_session_text(const SessionRecord &session, bool pretty) {
    auto encoded = encode_session(session);
    if (!encoded)
        return encoded.error();
    bool ok = true;
    std::string text = encoded.value().dump(pretty, 2, &ok);
    if (!ok)
        return storage_error("failed to serialize session", ErrorCategory::Internal);
    return text;
}

Result<SessionRecord> decode_session_text(std::string_view text, std::string_view source) {
    auto parsed = Json::parse(text, source);
    if (!parsed)
        return parsed.error();
    return decode_session(parsed.value());
}

Result<void> validate_session(const SessionRecord &session) {
    if (session.schema != mahak_result_schema_version)
        return storage_error("session schema is " + std::to_string(session.schema));
    if (session.id.empty())
        return storage_error("session id must not be empty");
    if (session.finished_unix_ns != 0 && session.created_unix_ns != 0 &&
        session.finished_unix_ns < session.created_unix_ns)
        return storage_error("session finished timestamp precedes its creation timestamp");
    static const std::vector<std::string> statuses = {"running", "completed", "failed",
                                                      "incomplete"};
    if (!session.status.empty() &&
        std::find(statuses.begin(), statuses.end(), session.status) == statuses.end())
        return storage_error("unknown session status '" + session.status + "'");
    for (const auto &run : session.runs) {
        if (run.case_id.empty())
            return storage_error("run without a case id");
        for (const auto &sample : run.samples) {
            if (sample.observation.metric.empty())
                return storage_error("sample in run '" + run.case_id + "' has no metric name");
            if (sample.observation.unit.empty())
                return storage_error("sample in run '" + run.case_id + "' has no metric unit");
        }
    }
    return {};
}

Result<SessionRecord> migrate_session(const Json &json, std::uint32_t from_version) {
    if (from_version == mahak_result_schema_version)
        return decode_session(json);
    if (from_version != 0)
        return storage_error("no migration path from result schema " +
                                 std::to_string(from_version),
                             ErrorCategory::Unsupported);
    // v0 was a flat list of runs with a "metrics" object per run.
    if (!json.is_object())
        return storage_error("v0 result document must be an object");
    SessionRecord session;
    session.schema = mahak_result_schema_version;
    session.status = "completed";
    if (const Json *id = json.find("id")) {
        if (const auto *string = id->as_string())
            session.id = *string;
    }
    if (session.id.empty())
        return storage_error("v0 result document has no id");
    if (const Json *created = json.find("created_unix_ns")) {
        if (const auto number = created->as_uint64())
            session.created_unix_ns = *number;
    }
    session.finished_unix_ns = session.created_unix_ns;
    const Json *runs = json.find("runs");
    const auto *array = runs != nullptr ? runs->as_array() : nullptr;
    if (array == nullptr)
        return storage_error("v0 result document has no runs array");
    for (const auto &entry : *array) {
        if (!entry.is_object())
            continue;
        RunRecord run;
        run.status = "completed";
        if (const Json *benchmark = entry.find("benchmark")) {
            if (const auto *string = benchmark->as_string())
                run.benchmark_id = *string;
        }
        if (const Json *target = entry.find("target")) {
            if (const auto *string = target->as_string())
                run.target_id = *string;
        }
        run.case_id = run.benchmark_id + "@" + run.target_id;
        if (const Json *metrics = entry.find("metrics")) {
            if (const auto *object = metrics->as_object()) {
                for (const auto &metric : *object) {
                    const auto value = metric.second.as_double();
                    if (!value)
                        continue;
                    SampleRecord sample;
                    sample.index = run.samples.size();
                    sample.repetition = sample.index;
                    sample.observation.metric = metric.first;
                    sample.observation.unit = "ns";
                    sample.observation.value = *value;
                    sample.observation.valid = true;
                    sample.observation.validity = Validity::Valid;
                    sample.observation.sequence = sample.index;
                    sample.measurement.wall_ns = static_cast<std::uint64_t>(*value);
                    run.samples.push_back(std::move(sample));
                }
            }
        }
        session.runs.push_back(std::move(run));
    }
    session.notes.push_back("migrated from result schema 0");
    return session;
}

Result<std::string> session_csv(const SessionRecord &session) {
    std::string csv =
        "session_id,manifest_id,run_case,benchmark,target,configuration,metric,unit,sequence,"
        "repetition,valid,validity,value,wall_ns,cpu_user_ns,cpu_system_ns,max_rss_bytes\n";
    for (const auto &run : session.runs) {
        for (const auto &sample : run.samples) {
            const auto &observation = sample.observation;
            csv += csv_escape(session.id);
            csv += ',';
            csv += csv_escape(session.manifest_id);
            csv += ',';
            csv += csv_escape(run.case_id);
            csv += ',';
            csv += csv_escape(run.benchmark_id);
            csv += ',';
            csv += csv_escape(run.target_id);
            csv += ',';
            csv += csv_escape(run.configuration);
            csv += ',';
            csv += csv_escape(observation.metric);
            csv += ',';
            csv += csv_escape(observation.unit);
            csv += ',';
            csv += std::to_string(observation.sequence);
            csv += ',';
            csv += std::to_string(observation.repetition);
            csv += ',';
            csv += observation.valid ? "1" : "0";
            csv += ',';
            csv += name(observation.validity);
            csv += ',';
            csv += std::to_string(observation.value);
            csv += ',';
            csv += std::to_string(sample.measurement.wall_ns);
            csv += ',';
            csv += std::to_string(sample.measurement.cpu.user_ns);
            csv += ',';
            csv += std::to_string(sample.measurement.cpu.system_ns);
            csv += ',';
            csv += std::to_string(sample.measurement.usage.max_rss_bytes);
            csv += '\n';
        }
    }
    return csv;
}

std::string result_schema_json() {
    Json schema = Json::object();
    schema.set("$schema", Json("https://json-schema.org/draft/2020-12/schema"));
    schema.set("$id", Json("https://libstone.invalid/mahak/session-v1.schema.json"));
    schema.set("title", Json("Mahak SessionRecord v1"));
    schema.set("type", Json("object"));
    Json required = Json::array();
    required.push(Json("schema"));
    required.push(Json("id"));
    required.push(Json("runs"));
    schema.set("required", std::move(required));
    Json properties = Json::object();
    Json schema_property = Json::object();
    schema_property.set("type", Json("integer"));
    schema_property.set("const", Json(static_cast<std::int64_t>(mahak_result_schema_version)));
    properties.set("schema", std::move(schema_property));
    Json id_property = Json::object();
    id_property.set("type", Json("string"));
    id_property.set("minLength", Json(1));
    properties.set("id", std::move(id_property));
    Json status_property = Json::object();
    status_property.set("type", Json("string"));
    Json statuses = Json::array();
    for (const char *status : {"running", "completed", "failed", "incomplete"})
        statuses.push(Json(status));
    status_property.set("enum", std::move(statuses));
    properties.set("status", std::move(status_property));
    Json runs_property = Json::object();
    runs_property.set("type", Json("array"));
    properties.set("runs", std::move(runs_property));
    Json notes_property = Json::object();
    notes_property.set("type", Json("array"));
    properties.set("notes", std::move(notes_property));
    schema.set("properties", std::move(properties));
    schema.set("additionalProperties", Json(true));
    return schema.dump(true);
}

std::string session_digest(const SessionRecord &session) {
    auto text = encode_session_text(session, false);
    if (!text)
        return {};
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : text.value()) {
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

struct HistoryStore::Impl {
    std::string root;
    std::string sessions_directory;
    std::string index_path;
    std::string lock_path;
};

HistoryStore::HistoryStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HistoryStore::~HistoryStore() = default;
HistoryStore::HistoryStore(HistoryStore &&) noexcept = default;
HistoryStore &HistoryStore::operator=(HistoryStore &&) noexcept = default;

Result<HistoryStore> HistoryStore::open(std::string root) {
    if (root.empty())
        return storage_error("history store root must not be empty");
    auto impl = std::make_unique<Impl>();
    impl->root = std::move(root);
    impl->sessions_directory = impl->root + "/sessions";
    impl->index_path = impl->root + "/index.json";
    impl->lock_path = impl->root + "/store.lock";
    std::error_code error;
    fs::create_directories(impl->sessions_directory, error);
    if (error)
        return storage_error("cannot create history store: " + error.message(),
                             ErrorCategory::Internal);
    const int lock = ::open(impl->lock_path.c_str(), O_WRONLY | O_CREAT, 0600);
    if (lock >= 0)
        ::close(lock);
    return HistoryStore(std::move(impl));
}

const std::string &HistoryStore::root() const noexcept {
    return impl_->root;
}

Result<void> HistoryStore::commit(const SessionRecord &session) {
    if (!impl_)
        return storage_error("history store is not open", ErrorCategory::Internal);
    if (!safe_session_id(session.id))
        return storage_error("session id '" + session.id + "' is not a safe file name");
    if (auto validated = validate_session(session); !validated)
        return validated.error();

    const int lock = ::open(impl_->lock_path.c_str(), O_WRONLY | O_CREAT, 0600);
    if (lock < 0)
        return storage_error(std::string("cannot open store lock: ") + std::strerror(errno),
                             ErrorCategory::Internal);
    if (::flock(lock, LOCK_EX) != 0) {
        ::close(lock);
        return storage_error("cannot acquire store lock", ErrorCategory::Internal);
    }
    struct Unlock {
        int fd;
        ~Unlock() {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
    } unlock{lock};

    const fs::path path = fs::path(impl_->sessions_directory) / (session.id + ".json");
    if (fs::exists(path)) {
        auto existing = decode_session_text([&] {
            std::ifstream stream(path, std::ios::binary);
            std::string contents((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());
            return contents;
        }(), path.string());
        if (!existing)
            return existing.error();
        if (session_digest(existing.value()) == session_digest(session))
            return {};
        return storage_error("session '" + session.id +
                             "' already exists with different contents");
    }
    auto encoded = encode_session_text(session, true);
    if (!encoded)
        return encoded.error();
    if (!write_file_atomic(path, encoded.value()))
        return storage_error("cannot commit session '" + session.id + "': " +
                                 std::strerror(errno),
                             ErrorCategory::Internal);
    return rebuild_index();
}

Result<std::vector<SessionSummary>> HistoryStore::list() const {
    if (!impl_)
        return storage_error("history store is not open", ErrorCategory::Internal);
    std::vector<SessionSummary> summaries;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(impl_->sessions_directory, error)) {
        if (error)
            return storage_error("cannot list sessions: " + error.message(),
                                 ErrorCategory::Internal);
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;
        std::ifstream stream(entry.path(), std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());
        auto decoded = decode_session_text(contents, entry.path().string());
        if (!decoded)
            continue;
        const SessionRecord &session = decoded.value();
        SessionSummary summary;
        summary.id = session.id;
        summary.manifest_id = session.manifest_id;
        summary.manifest_version = session.manifest_version;
        summary.status = session.status;
        summary.created_unix_ns = session.created_unix_ns;
        summary.finished_unix_ns = session.finished_unix_ns;
        summary.run_count = session.runs.size();
        summary.host = session.host;
        summaries.push_back(std::move(summary));
    }
    std::sort(summaries.begin(), summaries.end(),
              [](const SessionSummary &left, const SessionSummary &right) {
                  if (left.created_unix_ns != right.created_unix_ns)
                      return left.created_unix_ns < right.created_unix_ns;
                  return left.id < right.id;
              });
    return summaries;
}

Result<SessionRecord> HistoryStore::load(std::string_view id) const {
    if (!impl_)
        return storage_error("history store is not open", ErrorCategory::Internal);
    if (!safe_session_id(id))
        return storage_error("'" + std::string(id) + "' is not a valid session id");
    const fs::path path = fs::path(impl_->sessions_directory) / (std::string(id) + ".json");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return storage_error("session '" + std::string(id) + "' not found",
                             ErrorCategory::NotFound);
    std::string contents((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
    return decode_session_text(contents, path.string());
}

Result<void> HistoryStore::remove(std::string_view id) {
    if (!impl_)
        return storage_error("history store is not open", ErrorCategory::Internal);
    if (!safe_session_id(id))
        return storage_error("'" + std::string(id) + "' is not a valid session id");
    const fs::path path = fs::path(impl_->sessions_directory) / (std::string(id) + ".json");
    std::error_code error;
    if (!fs::remove(path, error) || error)
        return storage_error("session '" + std::string(id) + "' not found",
                             ErrorCategory::NotFound);
    return rebuild_index();
}

Result<void> HistoryStore::recover() {
    if (!impl_)
        return storage_error("history store is not open", ErrorCategory::Internal);
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(impl_->sessions_directory, error)) {
        if (error)
            return storage_error("cannot scan sessions: " + error.message(),
                                 ErrorCategory::Internal);
        if (entry.path().extension() == ".tmp")
            fs::remove(entry.path(), error);
    }
    return rebuild_index();
}

Result<void> HistoryStore::rebuild_index() {
    if (!impl_)
        return storage_error("history store is not open", ErrorCategory::Internal);
    auto summaries = list();
    if (!summaries)
        return summaries.error();
    Json index = Json::object();
    index.set("schema", Json(1));
    Json sessions = Json::array();
    for (const auto &summary : summaries.value()) {
        Json entry = Json::object();
        entry.set("id", Json(summary.id));
        entry.set("manifest_id", Json(summary.manifest_id));
        entry.set("manifest_version", Json(summary.manifest_version));
        entry.set("status", Json(summary.status));
        entry.set("created_unix_ns", Json(static_cast<std::int64_t>(summary.created_unix_ns)));
        entry.set("finished_unix_ns", Json(static_cast<std::int64_t>(summary.finished_unix_ns)));
        entry.set("run_count", Json(static_cast<std::int64_t>(summary.run_count)));
        entry.set("host", Json(summary.host));
        sessions.push(std::move(entry));
    }
    index.set("sessions", std::move(sessions));
    if (!write_file_atomic(impl_->index_path, index.dump(true)))
        return storage_error("cannot write history index", ErrorCategory::Internal);
    return {};
}

} // namespace v1
} // namespace moonstone::mahak
