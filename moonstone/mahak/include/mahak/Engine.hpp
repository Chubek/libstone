#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Model.hpp"
#include "Manifest.hpp"

namespace moonstone::mahak {
inline namespace v1 {

struct EngineOptions {
    std::string work_root; // empty selects a private directory under TMPDIR
    bool keep_work_directories = false;
    bool verbose = false;
    // Contended execution is opt-in and is recorded as a measurement condition.
    bool allow_concurrent = false;
    bool use_perf = false;
    std::vector<std::string> perf_events;
    std::size_t max_output_bytes = 1u << 20;
    std::uint64_t timeout_ms = 0;
    std::vector<int> cpu_affinity;
    std::uint32_t max_repetitions = 0;
    std::string session_id;
};

// Embeddable benchmark engine. One Engine owns at most one active session at a
// time; concurrent run() calls are rejected. cancel() is thread-safe and makes
// the active child process group terminate.
class Engine {
  public:
    explicit Engine(EngineOptions options = {});
    ~Engine();
    Engine(Engine &&) noexcept;
    Engine &operator=(Engine &&) noexcept;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    Result<EnvironmentInfo> environment() const;

    // Executes every stage of a planned case. Build/setup failures are returned
    // in CaseResult, not as a harness error. Only invalid plans or harness
    // failures produce an Error.
    Result<CaseResult> run_case(const CasePlan &plan);

    // Runs a plan and returns a complete SessionRecord. Partial results are
    // preserved on failure; status is "failed" or "incomplete".
    Result<SessionRecord> run(const SuitePlan &plan, std::string session_id = {});

    Result<SessionRecord> run_file(std::string_view manifest_path,
                                   const PlanOptions &options = {});

    // Loads, validates, plans and runs in one call. Used by the C ABI.
    Result<SessionRecord> run_manifest_text(std::string_view text, std::string_view source,
                                            std::string base_directory,
                                            const PlanOptions &options = {});

    void cancel() noexcept;
    bool cancelled() const noexcept;
    const EngineOptions &options() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace v1
} // namespace moonstone::mahak
