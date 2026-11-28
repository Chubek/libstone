#include "mahak/Engine.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "SchedulerInternal.hpp"
#include "mahak/Execution.hpp"
#include "mahak/Manifest.hpp"
#include "mahak/Measurement.hpp"
#include "mahak/Plugin.hpp"
#include "mahak/Statistics.hpp"

namespace moonstone::mahak {
inline namespace v1 {
namespace {

namespace fs = std::filesystem;

Error engine_error(std::string message, ErrorCategory category = ErrorCategory::Internal) {
    Error error;
    error.category = category;
    error.message = std::move(message);
    return error;
}

std::string host_name() {
    char buffer[256] = {0};
    if (::gethostname(buffer, sizeof(buffer) - 1) != 0)
        return "unknown";
    return buffer;
}

std::string random_hex(std::size_t digits) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string out;
    out.reserve(digits);
    std::uint64_t state = static_cast<std::uint64_t>(monotonic_now_ns()) ^
                          (static_cast<std::uint64_t>(::getpid()) << 32);
    for (std::size_t index = 0; index < digits; ++index) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out.push_back(alphabet[state & 0xF]);
    }
    return out;
}

std::string trim_copy(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n' ||
                             text.front() == '\r' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' ||
                             text.back() == '\r' || text.back() == '\t'))
        text.remove_suffix(1);
    return std::string(text);
}

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string configuration_of(const CasePlan &plan) {
    std::string text;
    for (const auto &parameter : plan.parameters) {
        if (!text.empty())
            text += ",";
        text += parameter.name + "=" + parameter.text;
    }
    return text;
}

OracleResult evaluate_oracle(const OracleSpec &oracle, const CommandResult &command) {
    OracleResult result;
    result.validity = Validity::InvalidOutput;
    if (command.spawn_failed) {
        result.message = "command failed to start";
        return result;
    }
    const std::string output = trim_copy(command.stdout_text);
    const auto finish = [&result](bool passed) {
        result.passed = passed;
        result.validity = passed ? Validity::Valid : Validity::InvalidOutput;
        return result;
    };
    switch (oracle.kind) {
    case OracleKind::None:
        return finish(true);
    case OracleKind::ExitCode: {
        const bool passed = command.exit_code == oracle.expected_exit && command.signal == 0;
        if (!passed)
            result.message = "expected exit " + std::to_string(oracle.expected_exit) +
                             ", observed " + std::to_string(command.exit_code);
        return finish(passed);
    }
    case OracleKind::StdoutExact:
        if (output != oracle.expected)
            result.message = "stdout did not match the expected text";
        return finish(output == oracle.expected);
    case OracleKind::StdoutContains:
        if (output.find(oracle.expected) == std::string::npos)
            result.message = "stdout does not contain the expected text";
        return finish(output.find(oracle.expected) != std::string::npos);
    case OracleKind::Regex: {
        bool passed = false;
        try {
            passed = std::regex_search(output, std::regex(oracle.pattern));
        } catch (const std::regex_error &error) {
            result.message = std::string("invalid regex: ") + error.what();
            result.validity = Validity::Unsupported;
            return result;
        }
        if (!passed)
            result.message = "stdout does not match the expected pattern";
        return finish(passed);
    }
    case OracleKind::Checksum: {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%016llx",
                      static_cast<unsigned long long>(fnv1a(output)));
        std::string expected = oracle.expected;
        std::transform(expected.begin(), expected.end(), expected.begin(),
                       [](unsigned char character) { return std::tolower(character); });
        const bool passed = buffer == expected;
        if (!passed)
            result.message = std::string("checksum mismatch: expected ") + oracle.expected +
                             ", observed " + buffer;
        return finish(passed);
    }
    case OracleKind::Tolerance: {
        static constexpr const char *digits = "0123456789+-.eE";
        const std::size_t end = output.find_last_not_of(" \t\r\n");
        if (end == std::string::npos) {
            result.message = "no numeric output to compare";
            return result;
        }
        const std::size_t begin = output.find_last_not_of(digits, end);
        const std::string number =
            output.substr(begin == std::string::npos ? 0 : begin + 1,
                          end - (begin == std::string::npos ? 0 : begin + 1) + 1);
        try {
            const bool passed = std::fabs(std::stod(number) - std::stod(oracle.expected)) <=
                                oracle.tolerance;
            if (!passed)
                result.message = "observed " + number + " outside tolerance of " + oracle.expected;
            return finish(passed);
        } catch (...) {
            result.message = "could not parse numeric output or expected value";
            return result;
        }
    }
    case OracleKind::JsonField: {
        auto parsed = Json::parse(command.stdout_text, "stdout");
        if (!parsed || !parsed.value().is_object()) {
            result.message = "stdout is not a JSON object";
            return result;
        }
        const Json *current = &parsed.value();
        std::string_view path = oracle.field;
        while (true) {
            const std::size_t dot = path.find('.');
            const std::string key(path.substr(0, dot));
            const Json *next = current->find(key);
            if (next == nullptr) {
                result.message = "field '" + oracle.field + "' not present in output";
                return result;
            }
            current = next;
            if (dot == std::string_view::npos)
                break;
            path.remove_prefix(dot + 1);
        }
        bool passed = false;
        if (const auto *text = current->as_string())
            passed = *text == oracle.expected;
        else if (const auto number = current->as_double())
            passed = std::fabs(*number - std::atof(oracle.expected.c_str())) <= oracle.tolerance;
        else
            passed = current->dump() == oracle.expected;
        if (!passed)
            result.message = "field '" + oracle.field + "' did not match";
        return finish(passed);
    }
    }
    return finish(false);
}

struct MetricDefinition {
    std::string name;
    std::string unit;
    MetricDirection direction;
};

std::vector<MetricDefinition> metrics_for(const CasePlan &plan) {
    std::vector<MetricDefinition> metrics;
    std::vector<std::string> seen;
    auto add = [&](const std::string &name, const std::string &unit,
                   MetricDirection direction) {
        if (std::find(seen.begin(), seen.end(), name) != seen.end())
            return;
        seen.push_back(name);
        metrics.push_back({name, unit, direction});
    };
    for (const auto &metric : plan.metrics)
        add(metric.name, metric.unit, metric.direction);
    add("wall", "ns", MetricDirection::LowerIsBetter);
    add("cpu_user", "ns", MetricDirection::LowerIsBetter);
    add("cpu_system", "ns", MetricDirection::LowerIsBetter);
    add("max_rss", "bytes", MetricDirection::LowerIsBetter);
    return metrics;
}

double metric_value(const MetricDefinition &metric, const CommandResult &command,
                    const MetricParseResult &parsed) {
    if (metric.name == "wall")
        return static_cast<double>(command.wall_ns);
    if (metric.name == "cpu_user")
        return static_cast<double>(command.cpu.user_ns);
    if (metric.name == "cpu_system")
        return static_cast<double>(command.cpu.system_ns);
    if (metric.name == "max_rss")
        return static_cast<double>(command.usage.max_rss_bytes);
    for (const auto &runtime : parsed.runtime) {
        if (runtime.name == metric.name)
            return runtime.value;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

} // namespace

struct Engine::Impl {
    EngineOptions options;
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    bool active = false;

    std::once_flag plugin_init;
    std::optional<PluginRegistry> plugin_registry;
    std::optional<Error> plugin_error;

    explicit Impl(EngineOptions value) : options(std::move(value)) {}

    Result<OracleResult> judge(const CasePlan &plan, const CommandResult &child) {
        if (plan.oracle.plugin.empty())
            return evaluate_oracle(plan.oracle, child);
        std::call_once(plugin_init, [&] {
            auto registry = PluginRegistry::create();
            if (!registry) {
                plugin_error = registry.error();
                return;
            }
            for (const auto &path : options.plugins) {
                auto loaded = registry.value().load(path);
                if (!loaded) {
                    plugin_error = loaded.error();
                    return;
                }
            }
            plugin_registry.emplace(std::move(registry.value()));
        });
        if (plugin_error)
            return *plugin_error;
        return plugin_registry->judge(plan.oracle.plugin, plan, child);
    }
};

Engine::Engine(EngineOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Engine::~Engine() = default;
Engine::Engine(Engine &&) noexcept = default;
Engine &Engine::operator=(Engine &&) noexcept = default;

const EngineOptions &Engine::options() const noexcept {
    return impl_->options;
}

Result<EnvironmentInfo> Engine::environment() const {
    return probe_environment();
}

void Engine::cancel() noexcept {
    if (impl_)
        impl_->cancelled.store(true);
}

bool Engine::cancelled() const noexcept {
    return impl_ && impl_->cancelled.load();
}

Result<CaseResult> Engine::run_case(const CasePlan &plan) {
    if (!impl_)
        return engine_error("engine is not initialized");
    if (plan.stages.empty())
        return engine_error("case '" + plan.id + "' has no stages", ErrorCategory::InvalidInput);

    CaseResult result;
    result.plan = plan;
    result.status = "unsupported";
    result.started_unix_ns = unix_now_ns();
    const std::uint64_t started = monotonic_now_ns();

    std::string work = plan.work_directory;
    if (work.empty()) {
        const std::string pattern =
            impl_->options.work_root.empty()
                ? (fs::temp_directory_path() / "mahak-XXXXXX").string()
                : (fs::path(impl_->options.work_root) / "mahak-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (::mkdtemp(buffer.data()) == nullptr)
            return engine_error(std::string("cannot create work directory: ") +
                                    std::strerror(errno),
                                ErrorCategory::Internal);
        work = buffer.data();
    } else {
        std::error_code error;
        fs::create_directories(work, error);
        if (error)
            return engine_error("cannot create work directory '" + work + "': " + error.message());
        ::chmod(work.c_str(), 0700);
    }
    const bool cleanup = !impl_->options.keep_work_directories;
    auto remove_work = [&] {
        if (!cleanup)
            return;
        std::error_code error;
        fs::remove_all(work, error);
    };

    for (const auto &fixture : plan.fixtures) {
        const fs::path destination = fs::path(work) / "fixtures" / fixture.first;
        std::error_code error;
        fs::create_directories(destination.parent_path(), error);
        if (!error && !fs::exists(fixture.second))
            error = std::make_error_code(std::errc::no_such_file_or_directory);
        if (!error)
            fs::copy_file(fixture.second, destination, fs::copy_options::overwrite_existing,
                          error);
        if (error) {
            result.status = "setup_failed";
            result.message = "cannot stage fixture '" + fixture.first + "' from '" +
                             fixture.second + "': " + error.message();
            remove_work();
            return result;
        }
    }

    ProcessOptions process;
    process.max_output_bytes = impl_->options.max_output_bytes;
    process.timeout_ms = impl_->options.timeout_ms;
    process.cancel = &impl_->cancelled;
    if (impl_->options.use_perf)
        process.perf_events = impl_->options.perf_events;

    std::map<std::string, std::uint64_t> sequences;
    std::uint64_t sample_index = 0;
    std::uint64_t measured_repetitions = 0;
    bool execute_stage_ran = false;
    const std::vector<MetricDefinition> metrics = metrics_for(plan);

    auto record_execute = [&](const CommandResult &child, bool measured) -> Result<bool> {
        auto judged = impl_->judge(plan, child);
        if (!judged)
            return judged.error();
        const MetricParseResult parsed = collect_command_metrics(child);
        const OracleResult &oracle = judged.value();
        result.oracle = oracle;
        const bool valid = child.validity == Validity::Valid && oracle.passed;
        result.status = valid ? "completed"
                              : (child.timed_out ? "failed" : "validation_failed");
        result.message = oracle.passed ? child.message : oracle.message;
        if (!measured)
            return valid;
        for (const auto &metric : metrics) {
            const double value = metric_value(metric, child, parsed);
            Observation observation;
            observation.metric = metric.name;
            observation.unit = metric.unit;
            observation.direction = metric.direction;
            observation.value = std::isnan(value) ? 0.0 : value;
            observation.sequence = sequences[metric.name]++;
            observation.repetition = measured_repetitions;
            observation.valid = valid && !std::isnan(value);
            observation.validity =
                observation.valid ? Validity::Valid
                                  : (oracle.passed ? Validity::MeasurementFailed
                                                   : (plan.oracle.plugin.empty()
                                                          ? Validity::InvalidOutput
                                                          : oracle.validity));
            observation.provenance =
                metric.name == "wall" || metric.name == "cpu_user" ||
                        metric.name == "cpu_system" || metric.name == "max_rss"
                    ? "harness"
                    : "runtime";
            SampleRecord sample;
            sample.index = sample_index++;
            sample.repetition = observation.repetition;
            sample.observation = observation;
            sample.measurement.wall_ns = child.wall_ns;
            sample.measurement.cpu = child.cpu;
            sample.measurement.usage = child.usage;
            sample.measurement.perf = child.perf;
            sample.measurement.phases = parsed.phases;
            sample.measurement.runtime = parsed.runtime;
            sample.configuration = configuration_of(plan);
            result.samples.push_back(std::move(sample));
        }
        ++measured_repetitions;
        return valid;
    };

    auto record_build = [&](StageKind kind, const CommandResult &child) {
        Observation build;
        build.metric = std::string(name(kind)) + "_wall";
        build.unit = "ns";
        build.direction = MetricDirection::LowerIsBetter;
        build.value = static_cast<double>(child.wall_ns);
        build.sequence = sequences[build.metric]++;
        build.valid = child.validity == Validity::Valid;
        build.validity = child.validity;
        build.provenance = "harness:stage-wall";
        SampleRecord sample;
        sample.index = sample_index++;
        sample.observation = build;
        sample.measurement.wall_ns = child.wall_ns;
        sample.measurement.cpu = child.cpu;
        sample.measurement.usage = child.usage;
        sample.configuration = configuration_of(plan);
        result.samples.push_back(std::move(sample));
    };

    auto prepare = [&](const CommandPlan &command_plan) {
        CommandPlan command = command_plan;
        if (command.working_directory.empty())
            command.working_directory = work;
        process.timeout_ms =
            command.timeout_ms != 0 ? command.timeout_ms : impl_->options.timeout_ms;
        return command;
    };

    static const StageKind order[] = {StageKind::Setup,   StageKind::Compile, StageKind::Link,
                                      StageKind::Validate, StageKind::Execute,
                                      StageKind::Teardown};
    for (StageKind kind : order) {
        for (const auto &stage : plan.stages) {
            if (stage.kind != kind)
                continue;
            if (impl_->cancelled.load()) {
                result.status = "incomplete";
                result.message = std::string("cancelled before stage ") + name(kind);
                result.duration_ns = monotonic_now_ns() - started;
                remove_work();
                return result;
            }
            StageResult stage_result;
            stage_result.kind = kind;
            bool stage_failed = false;

            if (kind == StageKind::Execute && stage.commands.size() == 1) {
                ScheduleOptions schedule;
                schedule.policy = plan.sampling;
                schedule.seed = plan.seed != 0 ? plan.seed : plan.sampling.seed;
                schedule.hard_max_repetitions = impl_->options.max_repetitions;
                RepetitionScheduler scheduler(schedule);
                const CommandPlan command = prepare(stage.commands.front());
                while (true) {
                    const bool warmup = scheduler.in_warmup();
                    if (!scheduler.next())
                        break;
                    auto executed = run_command(command, process);
                    if (!executed) {
                        result.status = "build_failed";
                        result.message = executed.error().message;
                        result.stages.push_back(std::move(stage_result));
                        result.duration_ns = monotonic_now_ns() - started;
                        remove_work();
                        return result;
                    }
                    CommandResult child = executed.value();
                    execute_stage_ran = true;
                    auto verdict = record_execute(child, !warmup);
                    if (!verdict) {
                        remove_work();
                        return verdict.error();
                    }
                    const bool valid = verdict.value();
                    const std::uint64_t wall = child.wall_ns;
                    stage_result.commands.push_back(std::move(child));
                    if (warmup)
                        continue;
                    scheduler.observe(wall, {static_cast<double>(wall)});
                    if (!valid) {
                        stage_failed = true;
                        break;
                    }
                }
            } else {
                for (const auto &command_plan : stage.commands) {
                    const CommandPlan command = prepare(command_plan);
                    auto executed = run_command(command, process);
                    if (!executed) {
                        result.status = "build_failed";
                        result.message = executed.error().message;
                        result.stages.push_back(std::move(stage_result));
                        result.duration_ns = monotonic_now_ns() - started;
                        remove_work();
                        return result;
                    }
                    CommandResult child = executed.value();
                    if (kind == StageKind::Compile || kind == StageKind::Link)
                        record_build(kind, child);
                    if (kind == StageKind::Execute) {
                        execute_stage_ran = true;
                        auto verdict = record_execute(child, true);
                        if (!verdict) {
                            remove_work();
                            return verdict.error();
                        }
                        if (!verdict.value())
                            stage_failed = true;
                    } else if (child.validity != Validity::Valid) {
                        stage_failed = true;
                        result.message = std::string(name(kind)) + " stage failed: " + child.message;
                        const std::string stderr_text = trim_copy(child.stderr_text);
                        if (!stderr_text.empty())
                            result.message += ": " + stderr_text.substr(0, 2000);
                    }
                    stage_result.commands.push_back(std::move(child));
                }
            }

            result.stages.push_back(std::move(stage_result));
            if (stage_failed && kind != StageKind::Teardown) {
                result.status = kind == StageKind::Execute ? "validation_failed" : "build_failed";
                result.duration_ns = monotonic_now_ns() - started;
                remove_work();
                return result;
            }
        }
    }
    if (!execute_stage_ran) {
        // A compile-only or link-only case never evaluates the oracle. Say so
        // explicitly instead of leaving a failed-looking default oracle.
        result.oracle.passed = true;
        result.oracle.validity = Validity::Valid;
        result.oracle.message = "no execute stage; oracle not evaluated";
    }
    if (result.status == "unsupported")
        result.status = "completed";
    result.duration_ns = monotonic_now_ns() - started;
    remove_work();
    return result;
}

Result<SessionRecord> Engine::run(const SuitePlan &plan, std::string session_id) {
    if (!impl_)
        return engine_error("engine is not initialized");
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->active)
        return engine_error("engine is already running a session", ErrorCategory::BudgetExhausted);
    struct ActiveGuard {
        Impl *impl;
        ~ActiveGuard() { impl->active = false; }
    } active_guard{impl_.get()};
    impl_->active = true;
    impl_->cancelled.store(false);

    SessionRecord session;
    session.id = session_id.empty() ? random_hex(16) : std::move(session_id);
    session.manifest_id = plan.manifest_id;
    session.manifest_version = plan.manifest_version;
    session.created_unix_ns = unix_now_ns();
    session.host = host_name();
    session.environment = probe_environment();
    session.status = "running";

    bool cancelled = false;
    bool failed = false;
    for (const auto &case_plan : plan.cases) {
        if (impl_->cancelled.load()) {
            cancelled = true;
            break;
        }
        auto case_result = run_case(case_plan);
        if (!case_result) {
            failed = true;
            session.notes.push_back("case '" + case_plan.id +
                                    "' failed: " + case_result.error().message);
            continue;
        }
        CaseResult &result = case_result.value();
        RunRecord run;
        run.case_id = result.plan.id;
        run.benchmark_id = result.plan.benchmark_id;
        run.target_id = result.plan.target_id;
        run.configuration = configuration_of(result.plan);
        run.parameters = result.plan.parameters;
        run.status = result.status;
        run.started_unix_ns = result.started_unix_ns;
        run.duration_ns = result.duration_ns;
        run.oracle = result.oracle;
        run.samples = std::move(result.samples);
        run.stages = std::move(result.stages);
        run.message = result.message;
        if (run.status != "completed")
            failed = true;
        session.runs.push_back(std::move(run));
    }
    session.finished_unix_ns = unix_now_ns();
    session.status = cancelled ? "incomplete" : (failed ? "failed" : "completed");
    if (cancelled)
        session.notes.push_back("session cancelled before all cases completed");
    return session;
}

Result<SessionRecord> Engine::run_file(std::string_view manifest_path,
                                       const PlanOptions &options) {
    auto manifest = load_manifest(manifest_path);
    if (!manifest)
        return manifest.error();
    const Diagnostics diagnostics = validate_manifest(manifest.value());
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.severity == Severity::Error)
            return engine_error(format_diagnostics(diagnostics), ErrorCategory::InvalidInput);
    }
    auto plan = plan_manifest(manifest.value(), options);
    if (!plan)
        return plan.error();
    auto session = run(plan.value());
    if (session)
        session.value().manifest_digest = manifest_digest(manifest.value());
    return session;
}

Result<SessionRecord> Engine::run_manifest_text(std::string_view text, std::string_view source,
                                                std::string base_directory,
                                                const PlanOptions &options) {
    auto manifest = parse_manifest(text, source, std::move(base_directory));
    if (!manifest)
        return manifest.error();
    const Diagnostics diagnostics = validate_manifest(manifest.value());
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.severity == Severity::Error)
            return engine_error(format_diagnostics(diagnostics), ErrorCategory::InvalidInput);
    }
    auto plan = plan_manifest(manifest.value(), options);
    if (!plan)
        return plan.error();
    auto session = run(plan.value());
    if (session)
        session.value().manifest_digest = manifest_digest(manifest.value());
    return session;
}

} // namespace v1
} // namespace moonstone::mahak
