#pragma once
#include "novostone/DatabaseBasedIR/DatabaseBasedIR.hpp"

#include <shared_mutex>
#include <atomic>

namespace novostone::speculative {
class Assumptions {
public:
  ir::Id assume(std::string description);
  void invalidate(ir::Id id);
private:
  struct Entry { std::int64_t epoch; bool valid; std::string description; };
  mutable std::shared_mutex mutex_;
  std::map<ir::Id, Entry> entries_;
  ir::Id next_ = 1;
  friend class Version;
};
struct Specialization { ir::Id parameter; std::uint32_t expected; ir::Id assumption; };
struct Frame {
  ir::Id id;
  ir::Id block;
  std::size_t offset;
  std::vector<ir::Id> locals;
};
struct Guard {
  ir::Id id;
  ir::Id instruction;
  std::uint32_t argument;
  std::uint32_t expected;
  ir::Id assumption;
  std::int64_t epoch;
  Frame frame;
};
struct Execution {
  ir::Outcome outcome;
  std::optional<ir::Id> failed_guard;
  std::optional<Frame> recovery;
  std::string reason;
};
struct Statistics { std::uint64_t executions, deoptimizations, predicate_failures, invalidations, forced_failures; };
// Validates the precise supported refinement: guarded parameter -> constant.
void verify(const ccdb::Snapshot& baseline, const ccdb::Snapshot& specialized);
class Version {
public:
  static Version specialize(const ccdb::Snapshot& baseline, std::shared_ptr<Assumptions> assumptions,
                            const std::vector<Specialization>& specializations);
  Execution execute(const std::vector<std::uint32_t>& arguments,
                    std::vector<std::uint32_t> memory = {}, std::size_t budget = 100000,
                    std::optional<ir::Id> force_guard = {}) const;
  const ccdb::Snapshot& snapshot() const { return fast_; }
  const std::vector<Guard>& guards() const { return guards_; }
  Statistics statistics() const;
private:
  struct Counters {
    std::atomic<std::uint64_t> executions{0}, deoptimizations{0}, predicate{0}, invalidations{0}, forced{0};
  };
  Version(ccdb::Snapshot baseline, ccdb::Snapshot fast, std::shared_ptr<Assumptions> assumptions,
          std::vector<Guard> guards)
      : baseline_(std::move(baseline)), fast_(std::move(fast)), assumptions_(std::move(assumptions)), guards_(std::move(guards)) {}
  ccdb::Snapshot baseline_, fast_;
  std::shared_ptr<Assumptions> assumptions_;
  std::vector<Guard> guards_;
  // Copies of a version share its observation counters, not executable state.
  std::shared_ptr<Counters> counters_ = std::make_shared<Counters>();
};
} // namespace novostone::speculative
