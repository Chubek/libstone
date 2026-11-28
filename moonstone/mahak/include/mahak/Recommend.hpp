#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Model.hpp"
#include "Storage.hpp"

// Advisory recommendations.
//
// A recommendation is never a change that has already happened. It names an
// action, the facts that make it legal, the observations that make it
// interesting, and a benchmark that could confirm or refute it. Facts come from
// the oracle (the language and semantics expert); observations come from Mahak.
// A timing trend can never establish that a transformation preserves semantics,
// so legality and performance are kept in separate fields on purpose.
//
// Three sources produce recommendations and all of them are subject to the same
// checks: the pattern library, the SMT solver, and an optional language model.
// A model suggestion that cannot be validated is reported as such; it is never
// silently promoted to advice.

namespace moonstone::mahak {
inline namespace v1 {

inline constexpr std::uint32_t mahak_recommend_api_version = 1;

enum class FactKind {
    Language,      // the source language accepts this construct
    Semantics,     // what the construct means
    Prerequisite,  // a condition the pass requires
    Target,        // the target can encode the result
    Pass,          // the pass exists and is available
    Exclusion,     // the pass must not run in this configuration
    Evidence       // an observation, measurement or earlier result
};

const char *name(FactKind kind) noexcept;
std::optional<FactKind> parse_fact_kind(std::string_view text) noexcept;

// Facts are assertions about the world, with provenance. `trusted` marks facts
// the caller asserts without a proof; a recommendation that depends on one says
// so instead of claiming it was proved.
struct OracleFact {
    std::string id;            // qualified, stable
    FactKind kind = FactKind::Prerequisite;
    std::string statement;     // human-readable, quotable in a report
    std::string provenance;    // document, command, or solver run that produced it
    bool trusted = false;      // accepted without proof
};

struct OracleFacts {
    std::string language;                 // e.g. "C11"
    std::string semantics;                // e.g. "defined bitvectors, LP64"
    std::vector<std::string> prerequis;   // facts the oracle asserts
    std::vector<OracleFact> facts;
    std::vector<std::string> exclusions;  // passes that must not run
    std::vector<std::string> passes;      // passes known to exist
    std::string target;                   // e.g. "x86_64-linux-gnu"
    std::vector<std::string> evidence;    // session or observation ids
};

Result<void> validate_facts(const OracleFacts &facts);

enum class RecommendationSource { PatternLibrary, Solver, Model };

const char *name(RecommendationSource source) noexcept;
std::optional<RecommendationSource> parse_recommendation_source(std::string_view text) noexcept;

enum class RecommendationStatus {
    Feasible,        // constraints satisfied, experiment proposed
    Rejected,        // a fact forbids it; the reason is recorded
    NeedsEvidence,   // legal-looking but a prerequisite fact is missing
    Unknown,         // the solver could not decide
    BudgetExhausted  // the solver stopped at its budget; never a success
};

const char *name(RecommendationStatus status) noexcept;

enum class SolverOutcome { Satisfiable, Unsatisfiable, Unknown, BudgetExhausted };

const char *name(SolverOutcome outcome) noexcept;

struct Recommendation {
    std::string id;             // stable within a history
    RecommendationSource source = RecommendationSource::PatternLibrary;
    RecommendationStatus status = RecommendationStatus::NeedsEvidence;
    std::string action;         // the proposed pass or rewrite, e.g. "blackstone.phi-fold"
    std::vector<std::string> constraints;   // facts that gate the action
    std::vector<std::string> evidence;      // observation/session/fact ids
    std::string reasoning;                  // why, in one quotable paragraph
    double uncertainty = 1.0;               // 0 certain .. 1 unknown; never implied by silence
    std::string provider;                   // "pattern-library", "satie", "model:<name>"
    std::string validation_benchmark;       // suite/case that would confirm or refute it
    SolverOutcome solver = SolverOutcome::Unknown;
};

struct RecommendOptions {
    bool enable_patterns = true;
    bool enable_solver = false;
    std::uint32_t solver_budget = 10000;  // decisions/conflicts, not seconds
    bool enable_model = false;            // requires an OpenAIPP provider
    std::string provider;                 // explicit; outranks the environment
    std::string model;
    std::size_t max_recommendations = 8;
    // Observations older than this, or produced by an incompatible fingerprint,
    // are labelled rather than dropped.
    std::uint64_t history_cutoff_unix_ns = 0;
};

class Recommender {
  public:
    static Result<Recommender> open(const RecommendOptions &options);
    ~Recommender();
    Recommender(Recommender &&) noexcept;
    Recommender &operator=(Recommender &&) noexcept;
    Recommender(const Recommender &) = delete;
    Recommender &operator=(const Recommender &) = delete;

    // Returns feasible, rejected and undecided recommendations together: a
    // rejection is information, and hiding it makes the advice unauditable.
    Result<std::vector<Recommendation>> recommend(const OracleFacts &facts,
                                                  const std::vector<SessionRecord> &history);

    // A model suggestion must pass these before it is reported as feasible.
    // Without them the recommendation is downgraded to NeedsEvidence.
    Result<Recommendation> admit(const Recommendation &candidate, const OracleFacts &facts);

    const RecommendOptions &options() const noexcept;

  private:
    Recommender();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace v1
} // namespace moonstone::mahak
