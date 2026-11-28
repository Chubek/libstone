#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Json.hpp"
#include "Version.hpp"

namespace moonstone::mahak {
inline namespace v1 {

using moonstone::Error;
using moonstone::ErrorCategory;
using moonstone::Result;

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------

enum class TargetKind { Compiler, Interpreter, VirtualMachine, Executable, Library, CustomCommand };
enum class StageKind { Setup, Compile, Link, Validate, Execute, Teardown };
enum class ExecutionMode { CompileOnly, LinkOnly, ExecuteOnly, EndToEnd, Startup, Throughput, Scalability };
enum class MeasurementMode { ColdProcess, WarmProcess, PersistentProcess, Concurrent };
enum class MetricDirection { LowerIsBetter, HigherIsBetter, Neutral };
enum class Validity {
    Valid,
    InvalidOutput,
    NonZeroExit,
    TimedOut,
    Crashed,
    Cancelled,
    Unsupported,
    SetupFailed,
    MeasurementFailed
};

const char *name(TargetKind value) noexcept;
const char *name(StageKind value) noexcept;
const char *name(ExecutionMode value) noexcept;
const char *name(MeasurementMode value) noexcept;
const char *name(MetricDirection value) noexcept;
const char *name(Validity value) noexcept;

std::optional<TargetKind> parse_target_kind(std::string_view text) noexcept;
std::optional<StageKind> parse_stage_kind(std::string_view text) noexcept;
std::optional<ExecutionMode> parse_execution_mode(std::string_view text) noexcept;
std::optional<MeasurementMode> parse_measurement_mode(std::string_view text) noexcept;
std::optional<MetricDirection> parse_metric_direction(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Manifest model (declarative, versioned, strict)
// ---------------------------------------------------------------------------

struct EnvironmentVariable {
    std::string name;
    std::string value;
    bool overwrite = true;
};

struct CommandSpec {
    std::vector<std::string> argv;
    std::string working_directory;
    std::vector<EnvironmentVariable> environment;
    std::optional<std::string> stdin_data;
    std::string stdin_file;
    std::vector<std::string> artifacts;
    // Zero means "use the suite default". A negative value is invalid.
    std::int64_t timeout_ms = 0;
};

struct StageSpec {
    StageKind kind = StageKind::Execute;
    std::vector<CommandSpec> commands;
    bool optional = false;
};

enum class OracleKind { None, ExitCode, StdoutExact, StdoutContains, Regex, Checksum, Tolerance, JsonField };

struct OracleSpec {
    OracleKind kind = OracleKind::ExitCode;
    int expected_exit = 0;
    std::string expected;
    std::string pattern;
    std::string field;
    double tolerance = 1e-9;
};

struct MetricSpec {
    std::string name;
    std::string unit;
    MetricDirection direction = MetricDirection::LowerIsBetter;
    bool primary = false;
};

struct SamplingPolicy {
    std::uint32_t warmup = 0;
    std::uint32_t repetitions = 1;
    std::uint64_t min_duration_ns = 0;
    std::uint64_t max_duration_ns = 0;
    std::uint32_t max_repetitions = 0;
    bool adaptive = false;
    double target_relative_stddev = 0.02;
    MeasurementMode measurement = MeasurementMode::WarmProcess;
    bool interleave = false;
    std::uint64_t seed = 0;
};

struct FixtureSpec {
    std::string name;
    std::string path;       // source-relative path
    std::string target_name; // logical artifact name copied into the work dir
    bool required = true;
};

struct ParameterSpec {
    std::string name;
    Json values = Json::array();
};

struct ExclusionSpec {
    std::string benchmark;
    std::string target;
    std::string reason;
};

struct TargetSpec {
    std::string id;
    TargetKind kind = TargetKind::Executable;
    std::string description;
    std::vector<StageSpec> stages;
    std::optional<CommandSpec> execute;
    std::vector<ParameterSpec> parameters;
    std::vector<EnvironmentVariable> environment;
};

struct BenchmarkSpec {
    std::string id;
    std::string description;
    std::string group;
    std::vector<std::string> tags;
    std::vector<ExecutionMode> modes;
    std::vector<FixtureSpec> fixtures;
    std::vector<ParameterSpec> parameters;
    std::vector<StageSpec> stages;
    std::optional<CommandSpec> execute;
    OracleSpec oracle;
    std::vector<MetricSpec> metrics;
    SamplingPolicy sampling;
    std::string work_directory;
};

struct Manifest {
    std::uint32_t schema = mahak_manifest_schema_version;
    std::string id;
    std::string version;
    std::string description;
    std::uint64_t seed = 0;
    std::string base_directory;
    std::string source_path;
    SamplingPolicy defaults;
    std::vector<TargetSpec> targets;
    std::vector<BenchmarkSpec> benchmarks;
    std::vector<ExclusionSpec> exclusions;
};

// A fully expanded, immutable benchmark case produced by the planner. All
// placeholders are resolved and file paths are absolute or relative to the
// case work directory.
struct ResolvedParameter {
    std::string name;
    Json value;
    std::string text;
};

struct CommandPlan {
    std::vector<std::string> argv;
    std::string working_directory;
    std::vector<EnvironmentVariable> environment;
    std::optional<std::string> stdin_data;
    std::vector<std::string> artifacts;
    std::int64_t timeout_ms = 0;
};

struct StagePlan {
    StageKind kind = StageKind::Execute;
    std::vector<CommandPlan> commands;
    bool optional = false;
};

struct CasePlan {
    std::string id;          // stable, derived from benchmark/target/parameters
    std::string benchmark_id;
    std::string target_id;
    std::string group;
    std::string description;
    std::vector<std::string> tags;
    std::vector<ExecutionMode> modes;
    std::vector<ResolvedParameter> parameters;
    std::vector<StagePlan> stages;
    OracleSpec oracle;
    std::vector<MetricSpec> metrics;
    SamplingPolicy sampling;
    std::map<std::string, std::string> fixtures;   // logical name -> source path
    std::map<std::string, std::string> artifacts;  // logical name -> path in work dir
    std::string work_directory;
    std::uint64_t seed = 0;
};

struct SuitePlan {
    std::string manifest_id;
    std::string manifest_version;
    std::string base_directory;
    std::uint64_t seed = 0;
    std::vector<CasePlan> cases;
};

// ---------------------------------------------------------------------------
// Measurement and environment model
// ---------------------------------------------------------------------------

struct CpuTimes {
    std::uint64_t user_ns = 0;
    std::uint64_t system_ns = 0;
};

struct ResourceUsage {
    std::uint64_t max_rss_bytes = 0;
    std::uint64_t minor_faults = 0;
    std::uint64_t major_faults = 0;
    std::uint64_t voluntary_context_switches = 0;
    std::uint64_t involuntary_context_switches = 0;
    std::uint64_t read_bytes = 0;  // zero when unavailable; see usage_notes
    std::uint64_t write_bytes = 0;
    std::vector<std::string> usage_notes;
};

struct PerfCounter {
    std::string name;
    std::uint64_t value = 0;
    bool enabled = false;
    bool scaled = false;
    double scale = 1.0;
    std::string note; // permission diagnostics, unsupported detail, multiplexing note
};

struct PhaseEvent {
    std::string name;
    std::string category; // compiler, runtime, gc, custom
    std::uint64_t start_ns = 0;
    std::uint64_t duration_ns = 0;
    std::int64_t value = 0;
    std::string detail;
};

struct RuntimeMetric {
    std::string name;
    std::string unit;
    double value = 0.0;
    std::string source; // provenance: stderr-json, prolific, custom
};

struct MeasurementSnapshot {
    std::uint64_t wall_ns = 0;
    CpuTimes cpu;
    ResourceUsage usage;
    std::vector<PerfCounter> perf;
    std::vector<PhaseEvent> phases;
    std::vector<RuntimeMetric> runtime;
    std::string clock = "CLOCK_MONOTONIC";
};

struct EnvironmentInfo {
    std::string os;
    std::string kernel;
    std::string architecture;
    std::string cpu_model;
    std::uint32_t logical_cpus = 0;
    std::uint32_t physical_cpus = 0;
    std::string cpu_frequency_policy;
    std::string affinity;
    std::string compiler;
    std::string compiler_version;
    std::string toolchain;
    std::map<std::string, std::string> extra;
};

// ---------------------------------------------------------------------------
// Execution results
// ---------------------------------------------------------------------------

struct CommandResult {
    std::vector<std::string> argv;
    int exit_code = -1;
    int signal = 0;
    bool timed_out = false;
    bool spawn_failed = false;
    bool output_truncated = false;
    std::string stdout_text;
    std::string stderr_text;
    std::uint64_t wall_ns = 0;
    CpuTimes cpu;
    ResourceUsage usage;
    std::vector<PerfCounter> perf;
    Validity validity = Validity::Valid;
    std::string message;
};

struct StageResult {
    StageKind kind = StageKind::Setup;
    std::vector<CommandResult> commands;
};

struct OracleResult {
    bool passed = false;
    Validity validity = Validity::InvalidOutput;
    std::string message;
};

struct Observation {
    std::string metric;
    std::string unit;
    MetricDirection direction = MetricDirection::LowerIsBetter;
    double value = 0.0;
    std::uint64_t sequence = 0;      // raw observation order within the metric
    std::uint64_t repetition = 0;
    bool valid = false;
    Validity validity = Validity::MeasurementFailed;
    std::string provenance;
};

struct SampleRecord {
    std::uint64_t index = 0;
    std::uint64_t repetition = 0;
    Observation observation;
    MeasurementSnapshot measurement;
    std::string configuration; // e.g. expanded parameter text
    std::uint64_t started_unix_ns = 0;
};

struct CaseResult {
    CasePlan plan;
    std::string status; // completed, setup_failed, build_failed, validation_failed, unsupported
    std::vector<StageResult> stages;
    std::vector<SampleRecord> samples;
    OracleResult oracle;
    std::string message;
    std::uint64_t started_unix_ns = 0;
    std::uint64_t duration_ns = 0;
};

// ---------------------------------------------------------------------------
// Statistics model (produced by Statistics.cpp, consumed by Storage/Report)
// ---------------------------------------------------------------------------

struct Estimates {
    std::size_t total = 0;
    std::size_t valid = 0;
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double median = 0.0;
    double variance = 0.0; // sample variance (n-1), zero for n < 2
    double standard_deviation = 0.0;
    double mad = 0.0; // median absolute deviation
    double q1 = 0.0;
    double q3 = 0.0;
    double p90 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double relative_standard_deviation = 0.0;
};

struct ConfidenceInterval {
    double lower = 0.0;
    double upper = 0.0;
    double confidence = 0.95;
    std::string method; // normal-t, bootstrap-percentile, unavailable
    bool defined = false;
};

struct BootstrapOptions {
    std::uint64_t seed = 0;
    std::uint32_t resamples = 2000;
    double confidence = 0.95;
    std::uint32_t max_resamples = 100000;
};

struct OutlierOptions {
    enum class Method { None, Mad } method = Method::Mad;
    double mad_threshold = 3.5;
    bool retain = true; // retained observations remain labelled in reports
};

struct OutlierReport {
    std::vector<std::size_t> excluded_indices;
    std::vector<Observation> retained;
    std::vector<Observation> excluded;
    std::string method;
    double threshold = 0.0;
};

struct RegressionCriteria {
    // Relative change required for practical significance. A regression also
    // requires the configured statistical evidence, unless disabled.
    double minimum_relative_change = 0.01; // 1%
    double practical_threshold = 0.05;     // 5% is "material" by default
    double confidence = 0.95;
    bool require_statistical_significance = true;
    bool require_both_samples_valid = true;
    std::uint32_t minimum_samples = 3;
};

struct ComparisonResult {
    std::string metric;
    std::string unit;
    MetricDirection direction = MetricDirection::LowerIsBetter;
    double baseline_mean = 0.0;
    double candidate_mean = 0.0;
    double baseline_median = 0.0;
    double candidate_median = 0.0;
    double absolute_change = 0.0;
    double percent_change = 0.0;
    double ratio = 0.0; // candidate / baseline
    double speedup = 0.0; // >1 means candidate faster for latency metrics
    double geometric_mean_ratio = 0.0;
    ConfidenceInterval interval;
    bool statistically_significant = false;
    bool practical_change = false;
    bool regression = false;
    bool improvement = false;
    std::string verdict; // regression, improvement, unchanged, insufficient-data, incompatible
    std::string message;
};

struct ComparisonOptions {
    bool paired = false;
    std::uint64_t seed = 0;
    std::uint32_t bootstrap_resamples = 2000;
    double confidence = 0.95;
    RegressionCriteria criteria;
    OutlierOptions outliers;
};

struct ScalabilityFit {
    bool defined = false;
    double exponent = 0.0;
    double intercept = 0.0;
    double r_squared = 0.0;
    std::size_t points = 0;
    std::string model; // log-log
};

// ---------------------------------------------------------------------------
// Persistent result model
// ---------------------------------------------------------------------------

struct RunRecord {
    std::string case_id;
    std::string benchmark_id;
    std::string target_id;
    std::string configuration;
    std::vector<ResolvedParameter> parameters;
    std::string status;
    std::uint64_t started_unix_ns = 0;
    std::uint64_t duration_ns = 0;
    OracleResult oracle;
    std::vector<SampleRecord> samples;
    std::vector<StageResult> stages;
    std::string message;
};

struct SessionRecord {
    std::uint32_t schema = mahak_result_schema_version;
    std::string id;
    std::string manifest_id;
    std::string manifest_version;
    std::string manifest_digest;
    std::uint64_t created_unix_ns = 0;
    std::uint64_t finished_unix_ns = 0;
    std::string status; // running, completed, failed, incomplete
    std::string host;
    EnvironmentInfo environment;
    std::vector<RunRecord> runs;
    std::vector<std::string> notes;
};

// ---------------------------------------------------------------------------
// Helpers shared by all modules
// ---------------------------------------------------------------------------

std::string expand_placeholders(std::string_view input,
                                const std::map<std::string, std::string> &variables,
                                bool *unresolved = nullptr);
std::string format_parameter_value(const Json &value);
std::string join_argv(const std::vector<std::string> &argv);
std::string metric_direction_name(MetricDirection direction) noexcept;

} // namespace v1
} // namespace moonstone::mahak
