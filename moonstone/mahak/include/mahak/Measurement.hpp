#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Model.hpp"

namespace moonstone::mahak {
inline namespace v1 {

// Monotonic wall clock in nanoseconds. This is the reference clock for all
// wall-time measurements; it never uses the realtime clock.
std::uint64_t monotonic_now_ns() noexcept;
std::uint64_t unix_now_ns() noexcept;

struct PerfAvailability {
    bool available = false;
    bool permission_granted = false;
    std::string reason;
    std::vector<std::string> supported_events;
};

// Probes perf_event_open without attributing failures to the benchmark.
PerfAvailability probe_perf();

struct PerfRequest {
    // Canonical event names mapped by Measurement.cpp (cycles, instructions,
    // branches, branch-misses, cache-references, cache-misses, and explicit
    // type:config pairs such as "raw:0x3c").
    std::vector<std::string> events;
    bool include_kernel = false;
};

// Linux-only counter session attached to a running child. stop() is idempotent.
// Unsupported events are returned enabled=false with a reason; they are never
// synthesized as zero. On non-Linux, attach() returns Unsupported.
class PerfSession {
  public:
    static Result<std::unique_ptr<PerfSession>> attach(std::int64_t pid, const PerfRequest &request);
    ~PerfSession();
    PerfSession(const PerfSession &) = delete;
    PerfSession &operator=(const PerfSession &) = delete;
    Result<std::vector<PerfCounter>> stop();

  private:
    struct Impl;
    explicit PerfSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Host, kernel, CPU, affinity and toolchain metadata. Unavailable fields are
// empty, never invented. `doctor` renders the reasons.
EnvironmentInfo probe_environment();

struct MetricParseResult {
    std::vector<PhaseEvent> phases;
    std::vector<RuntimeMetric> runtime;
    std::vector<std::string> warnings;
};

// Parses Mahak's documented metric-record protocol from stdout/stderr:
//   MAHAK-METRIC {"name":"gc_pause","unit":"ns","value":123}
//   MAHAK-PHASE  {"name":"optimize","category":"compiler","start_ns":10,"duration_ns":20}
// Unknown or malformed records are reported in warnings and ignored; they are
// never converted into measurements.
MetricParseResult parse_metric_records(std::string_view text, std::string_view source);

// Extracts metric records from both streams of a finished command and merges
// them without losing provenance.
MetricParseResult collect_command_metrics(const CommandResult &result);

} // namespace v1
} // namespace moonstone::mahak
