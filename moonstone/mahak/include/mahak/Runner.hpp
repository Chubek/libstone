#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Model.hpp"

// Runner scripts and reusable extensions.
//
// A runner script customizes one session: it may adjust runtime parameters,
// generate fixtures, judge output, define ad-hoc measurements, analyze
// completed observations, and render a report. It is intentionally the loose
// counterpart of a native plugin: a plugin is reviewed code with a stable id, a
// runner is per-experiment glue.
//
// Both are loaded through the same Lua library (lmahak). Extensions register
// hooks by qualified id and are reusable; a runner script is scoped to the
// session that named it. When both define a hook, extensions run first in
// registration order and the runner runs last, so a runner can override.
//
// JSON is not optional: every execution path still needs a manifest. A runner
// augments a manifest; it can never replace one. A script-only invocation is a
// configuration error.

namespace moonstone::mahak {
inline namespace v1 {

inline constexpr std::uint32_t mahak_runner_api_version = 1;

enum class HookKind { Configure, Generate, Judge, Measure, Analyze, Predict, Report };

const char *name(HookKind kind) noexcept;
std::optional<HookKind> parse_hook_kind(std::string_view text) noexcept;

// When several hooks answer the same question the composition must be named
// explicitly; an ambiguous judge is a configuration error, not a tie-break.
enum class HookComposition {
    // The last registered hook that returns a verdict wins (runner overrides).
    LastWins,
    // Every judge runs and all must pass; verdicts are ANDed.
    AllMustPass,
    // The first judge that passes wins; a strict majority is not implied.
    FirstPass
};

const char *name(HookComposition composition) noexcept;
std::optional<HookComposition> parse_hook_composition(std::string_view text) noexcept;

struct RunnerLimits {
    std::uint64_t max_instructions = 10'000'000;
    std::size_t max_memory_bytes = 64u * 1024u * 1024u;
    std::uint32_t max_output_bytes = 1u << 20;
    // Number of times one hook may be invoked for a single case.
    std::uint32_t max_invocations = 1024;
};

struct RunnerOptions {
    std::string path;                      // runner script; empty means none
    std::vector<std::string> extensions;   // reusable extension scripts or ids
    std::vector<std::string> search_paths; // where require() looks for modules
    std::uint64_t seed = 0;
    HookComposition judge = HookComposition::AllMustPass;
    RunnerLimits limits;
    bool verbose = false;
};

// Identity of the exact script set that shaped a session. Persisted with the
// session so a replay can refuse to reuse a result produced by other code.
struct RunnerFingerprint {
    bool valid = false;
    std::string digest;                  // FNV-1a 64 over the script set
    std::vector<std::string> scripts;    // "path:digest" in load order
    std::uint32_t api_version = mahak_runner_api_version;
};

struct RunnerDiagnostic {
    std::string hook;    // hook name, or "load"
    std::string script;  // script path
    std::string message;
    int line = 0;
};

class Runner {
  public:
    // Loads the runner and its extensions. An empty RunnerOptions::path with no
    // extensions yields a runner that answers has() false for every hook.
    static Result<Runner> open(const RunnerOptions &options);
    ~Runner();
    Runner(Runner &&) noexcept;
    Runner &operator=(Runner &&) noexcept;
    Runner(const Runner &) = delete;
    Runner &operator=(const Runner &) = delete;

    bool has(HookKind kind) const;
    std::vector<HookKind> hooks() const;
    Result<RunnerFingerprint> fingerprint() const;

    // Hook results are the host's own types, so a script cannot invent a
    // measurement shape or an oracle verdict the harness does not understand.
    Result<void> configure(CasePlan &plan);
    Result<std::string> generate(std::uint64_t seed);
    Result<OracleResult> judge(const CasePlan &plan, const CommandResult &command);
    Result<Observation> measure(const CasePlan &plan, const CommandResult &command);
    Result<Json> analyze(const SessionRecord &session);
    Result<Json> predict(const std::vector<Observation> &observations);
    Result<std::string> report(const SessionRecord &session);

    // Every diagnostic produced so far, in order. Diagnostics never change a
    // verdict; they explain one.
    const std::vector<RunnerDiagnostic> &diagnostics() const;
    void clear_diagnostics();

  private:
    Runner();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace v1
} // namespace moonstone::mahak
