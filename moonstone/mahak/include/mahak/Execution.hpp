#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Model.hpp"
#include "Rng.hpp"

namespace moonstone::mahak {
inline namespace v1 {

// ---------------------------------------------------------------------------
// Process supervision
// ---------------------------------------------------------------------------

struct ResourceLimits {
    // Zero means "leave unchanged". Limits are applied in the child before exec.
    std::uint64_t cpu_time_ms = 0;
    std::uint64_t address_space_bytes = 0;
    std::uint64_t file_size_bytes = 0;
    std::uint64_t open_files = 0;
    std::uint64_t processes = 0;
};

struct ProcessOptions {
    bool capture_output = true;
    std::size_t max_output_bytes = 1u << 20; // per stream; excess is discarded and flagged
    std::uint64_t timeout_ms = 0;            // zero disables the harness timeout
    bool use_process_group = true;           // kill the whole group on timeout/cancel
    std::vector<int> cpu_affinity;           // empty means unpinned
    bool isolate_environment = false;        // start from an empty environment
    ResourceLimits limits;
    bool collect_rusage = true;
    // Requested Linux perf events. Unsupported or unpermitted events are
    // reported in CommandResult::perf with enabled=false; they never become 0.
    std::vector<std::string> perf_events;
    // External cancellation. The supervisor terminates the process group.
    const std::atomic<bool> *cancel = nullptr;
};

// Runs one argv vector without a shell. The child receives argv[0] verbatim.
// Returns a CommandResult even for non-zero exit, signal, timeout and spawn
// failure; the Result is an error only for invalid input or harness failure.
Result<CommandResult> run_command(const CommandPlan &command, const ProcessOptions &options);

struct PersistentOptions {
    ProcessOptions process;
    std::string iteration_input; // written before each measurement; newline appended
    std::uint64_t iteration_timeout_ms = 0;
    std::uint32_t max_iterations = 0;
};

struct PersistentResult {
    CommandResult launch;
    std::vector<CommandResult> iterations;
    std::string note;
};

// Line-oriented persistent process measurement. The target must read one line
// per iteration and emit one newline-terminated result line. This is an
// explicit protocol, not a universal persistent-process adapter.
Result<PersistentResult> run_persistent(const CommandPlan &command,
                                        const PersistentOptions &options);

// ---------------------------------------------------------------------------
// Repetition scheduling
// ---------------------------------------------------------------------------

struct ScheduleOptions {
    SamplingPolicy policy;
    std::uint64_t seed = 0;
    std::uint32_t hard_max_repetitions = 0; // 0 selects a bounded default
};

class RepetitionScheduler {
  public:
    explicit RepetitionScheduler(ScheduleOptions options);

    // Returns the next repetition index, or nullopt when the plan is complete.
    std::optional<std::uint64_t> next();
    bool in_warmup() const noexcept;
    std::uint32_t warmup_completed() const noexcept;
    std::uint32_t measured_count() const noexcept;

    // Feed one measured duration and its (optional) primary values. Returns
    // true when adaptive stopping is allowed to finish the schedule early.
    bool observe(std::uint64_t elapsed_ns, const std::vector<double> &values);
    bool should_stop(const Estimates &estimates) const;

    // Deterministic interleaving of baseline/candidate schedule indices.
    std::vector<std::size_t> interleave(std::size_t count) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace v1
} // namespace moonstone::mahak
