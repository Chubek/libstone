#pragma once
#include "novostone/DatabaseBasedIR/DatabaseBasedIR.hpp"

namespace novostone::query {
struct Input {
  std::vector<std::uint32_t> arguments;
  std::vector<std::uint32_t> memory;
};
enum class Verdict { Passed, Mismatch, Inconclusive };
struct Counterexample { Input input; ir::Outcome before; ir::Outcome after; };
struct Validation {
  Verdict verdict = Verdict::Inconclusive;
  std::size_t matched = 0;
  std::size_t inconclusive = 0;
  std::optional<Counterexample> counterexample;
  std::string explain() const;
};
// Resource exhaustion is inconclusive, never evidence of equivalence.
Validation validate(const ir::Function& before, const ir::Function& after,
                    const std::vector<Input>& inputs, std::size_t step_budget = 100000);
struct ReductionOptions {
  std::size_t evaluations = 256; // Paired executions, including the initial witness.
  std::size_t step_budget = 100000; // Per execution, not a wall-clock deadline.
};
struct Reduction {
  Counterexample counterexample;
  std::size_t evaluations = 0;
  std::size_t inconclusive = 0;
  bool budget_exhausted = false;
  std::string explain() const;
};
// Rechecks the input, then greedily shrinks its memory suffix and integer values.
// Only completed mismatches are retained. No global minimality is claimed.
Reduction minimize(const ir::Function& before, const ir::Function& after,
                   Input input, ReductionOptions options = {});
std::vector<Input> corpus(std::uint32_t arity, std::uint32_t seed, std::size_t samples = 128,
                          std::size_t max_memory = 8);
struct Rewrite {
  ccdb::Revision revision;
  ir::Id instruction;
  std::string rule;
  std::set<std::string> dependencies;
};
struct Options {
  std::size_t rewrites = 1000;
  std::size_t query_work = 10000000;
  std::size_t query_facts = 1000000;
  std::size_t analysis_work = 1000000;
  bool conditional_constants = true;
};
enum class ValueKind { Unknown, Constant, Overdefined };
struct AbstractValue {
  ValueKind kind = ValueKind::Unknown;
  std::uint32_t value = 0;
  bool operator==(const AbstractValue&) const = default;
};
struct Propagation {
  std::map<ir::Id, AbstractValue> values;
  std::set<ir::Id> blocks;
  std::set<std::pair<ir::Id, ir::Id>> edges;
  std::size_t work = 0;
  std::string explain() const;
};
// Sparse conditional constant propagation. Only a completed fixed point is
// returned; budget exhaustion throws, so partial constants cannot justify edits.
Propagation propagate(const ir::Function& function, std::size_t work_budget = 1000000);
// Only the built-in semantics-preserving rewrite engine can construct candidates.
// Cost models receive these immutable objects, never an authority to waive legality.
class Candidate {
public:
  const ccdb::Snapshot& base() const { return base_; }
  const ccdb::Snapshot& result() const { return result_; }
  const std::vector<Rewrite>& rewrites() const { return rewrites_; }
  bool budget_exhausted() const { return exhausted_; }
  const std::string& stop_reason() const { return stop_reason_; }
  std::size_t query_work() const { return work_; }
  std::size_t analysis_work() const { return analysis_work_; }
  std::string explain() const;
private:
  Candidate(ccdb::Snapshot base, ccdb::Snapshot result, std::vector<Rewrite> rewrites, bool exhausted,
            std::string stop_reason = {}, std::size_t work = 0, std::size_t analysis_work = 0)
    : base_(std::move(base)), result_(std::move(result)), rewrites_(std::move(rewrites)), exhausted_(exhausted),
      stop_reason_(std::move(stop_reason)), work_(work), analysis_work_(analysis_work) {}
  ccdb::Snapshot base_, result_;
  std::vector<Rewrite> rewrites_;
  bool exhausted_;
  std::string stop_reason_;
  std::size_t work_;
  std::size_t analysis_work_;
  friend Candidate baseline(const ccdb::Snapshot&);
  friend Candidate optimize(const ccdb::Snapshot&, std::size_t);
  friend Candidate optimize(const ccdb::Snapshot&, Options);
};
Candidate baseline(const ccdb::Snapshot& snapshot);
Candidate optimize(const ccdb::Snapshot& snapshot, std::size_t rewrite_budget = 1000);
Candidate optimize(const ccdb::Snapshot& snapshot, Options options);
ccdb::Snapshot apply(ccdb::Database& database, const Candidate& candidate);
ccdb::Snapshot apply_checked(ccdb::Database& database, const Candidate& candidate,
                            const std::vector<Input>& inputs, std::size_t step_budget = 100000);
} // namespace novostone::query
