#include "novostone/SpeculativeIR/SpeculativeIR.hpp"

#include <algorithm>
#include <limits>
#include <mutex>

namespace novostone::speculative {
namespace {
std::int64_t number(const datalog::Value& v) { return std::get<std::int64_t>(v); }
std::vector<Guard> decode(const ccdb::Snapshot& snapshot) {
  std::map<ir::Id, Frame> frames;
  for (const auto& row : snapshot.relation("Frame")) {
    if (number(row[0]) <= 0 || number(row[2]) < 0 ||
        !frames.emplace(number(row[0]), Frame{number(row[0]), number(row[1]), static_cast<std::size_t>(number(row[2])), {}}).second)
      throw std::invalid_argument("invalid recovery frame");
  }
  for (const auto& row : snapshot.relation("Binding")) frames.at(number(row[0])).locals.push_back(number(row[1]));
  std::map<ir::Id, std::int64_t> epochs;
  for (const auto& row : snapshot.relation("Assumption"))
    if (number(row[0]) <= 0 || number(row[1]) <= 0 || !epochs.emplace(number(row[0]), number(row[1])).second)
      throw std::invalid_argument("invalid assumption record");
  std::vector<Guard> guards;
  std::set<ir::Id> ids, instructions, used_frames, used_assumptions;
  for (const auto& row : snapshot.relation("Guard")) {
    if (number(row[0]) <= 0 || !ids.insert(number(row[0])).second || !instructions.insert(number(row[1])).second ||
        number(row[2]) < 0 || number(row[2]) > UINT32_MAX || number(row[3]) < 0 || number(row[3]) > UINT32_MAX)
      throw std::invalid_argument("invalid or duplicate guard");
    guards.push_back({number(row[0]), number(row[1]), static_cast<std::uint32_t>(number(row[2])),
      static_cast<std::uint32_t>(number(row[3])), number(row[4]), epochs.at(number(row[4])), frames.at(number(row[5]))});
    if (!used_frames.insert(number(row[5])).second) throw std::invalid_argument("shared recovery frame");
    used_assumptions.insert(number(row[4]));
  }
  if (used_frames.size() != frames.size() || used_assumptions.size() != epochs.size()) throw std::invalid_argument("orphan recovery metadata");
  return guards;
}
}
ir::Id Assumptions::assume(std::string description) {
  std::unique_lock lock(mutex_);
  if (next_ == std::numeric_limits<ir::Id>::max()) throw std::overflow_error("assumption IDs exhausted");
  auto id = next_++;
  entries_.emplace(id, Entry{1, true, std::move(description)});
  return id;
}
void Assumptions::invalidate(ir::Id id) {
  std::unique_lock lock(mutex_);
  auto& entry = entries_.at(id);
  if (entry.valid) { entry.valid = false; ++entry.epoch; }
}
void verify(const ccdb::Snapshot& baseline, const ccdb::Snapshot& specialized) {
  ir::verify(baseline); ir::verify(specialized);
  auto original = ir::load(baseline), expected = original;
  auto guards = decode(specialized);
  std::set<ir::Id> found;
  for (auto& [block_id, block] : expected.blocks) {
    std::vector<ir::Id> available;
    for (std::size_t pos = 0; pos < block.instructions.size(); ++pos) {
      auto& i = block.instructions[pos];
      auto it = std::find_if(guards.begin(), guards.end(), [&](const auto& g) { return g.instruction == i.id; });
      if (it != guards.end()) {
        if (i.op != ir::Op::Param || i.immediate != it->argument || it->frame.block != block_id || it->frame.offset != pos)
          throw std::invalid_argument("guard does not dominate its parameter specialization");
        auto locals = it->frame.locals;
        auto sorted = available;
        std::sort(sorted.begin(), sorted.end());
        if (locals != sorted) throw std::invalid_argument("incomplete or unavailable frame bindings");
        i.op = ir::Op::Const; i.immediate = it->expected;
        found.insert(i.id);
      }
      if (ir::produces_value(i.op)) available.push_back(i.id);
    }
  }
  if (found.size() != guards.size() || ir::dump(expected) != ir::dump(ir::load(specialized)))
    throw std::invalid_argument("specialized code is not the guarded baseline refinement");
}
Version Version::specialize(const ccdb::Snapshot& baseline, std::shared_ptr<Assumptions> assumptions,
                            const std::vector<Specialization>& specs) {
  if (!assumptions) throw std::invalid_argument("missing assumption registry");
  ir::verify(baseline);
  for (const auto& name : {"Guard", "Frame", "Binding", "Assumption"})
    if (!baseline.relation(name).empty()) throw std::invalid_argument("baseline is already speculative");
  auto f = ir::load(baseline);
  std::shared_lock lock(assumptions->mutex_);
  ccdb::Database db(ir::program());
  auto tx = db.begin();
  std::set<ir::Id> specialized;
  ir::Id next = 1;
  for (const auto& spec : specs) {
    if (!specialized.insert(spec.parameter).second) throw std::invalid_argument("duplicate specialization");
    const auto& assumption = assumptions->entries_.at(spec.assumption);
    if (!assumption.valid) throw std::invalid_argument("invalidated assumption");
    bool found = false;
    for (auto& [block_id, block] : f.blocks) {
      std::vector<ir::Id> locals;
      for (std::size_t pos = 0; pos < block.instructions.size(); ++pos) {
        auto& i = block.instructions[pos];
        if (i.id == spec.parameter) {
          if (i.op != ir::Op::Param) throw std::invalid_argument("only parameters can be specialized");
          auto guard = next++, frame = next++;
          tx.insert("Guard", {guard, i.id, static_cast<std::int64_t>(i.immediate), static_cast<std::int64_t>(spec.expected), spec.assumption, frame});
          tx.insert("Frame", {frame, block_id, static_cast<std::int64_t>(pos)});
          for (auto value : locals) tx.insert("Binding", {frame, value});
          tx.insert("Assumption", {spec.assumption, assumption.epoch, assumption.description});
          i.op = ir::Op::Const; i.immediate = spec.expected;
          found = true;
        }
        if (ir::produces_value(i.op)) locals.push_back(i.id);
      }
    }
    if (!found) throw std::invalid_argument("missing specialization parameter");
  }
  ir::store(tx, f);
  auto fast = tx.commit("guarded parameter specialization", [&](const auto& s) { verify(baseline, s); });
  auto guards = decode(fast);
  return Version(baseline, fast, std::move(assumptions), std::move(guards));
}
Execution Version::execute(const std::vector<std::uint32_t>& args, std::vector<std::uint32_t> memory,
                           std::size_t budget, std::optional<ir::Id> force_guard) const {
  // Invalidation is a safepoint: it waits for executing versions, then all future
  // entries observe the invalid epoch. No unchecked interval races invalidation.
  std::shared_lock lock(assumptions_->mutex_);
  if (force_guard && std::none_of(guards_.begin(), guards_.end(), [&](const auto& g) { return g.id == *force_guard; }))
    throw std::invalid_argument("forced guard does not belong to this version");
  counters_->executions.fetch_add(1, std::memory_order_relaxed);
  auto fast = ir::load(fast_);
  ir::State state; state.block = fast.entry; state.memory = std::move(memory);
  const Guard* failed = nullptr;
  std::string reason;
  auto outcome = ir::resume(fast, args, std::move(state), budget, [&](const auto& i, const auto&) {
    auto guard = std::find_if(guards_.begin(), guards_.end(), [&](const auto& g) { return g.instruction == i.id; });
    if (guard == guards_.end()) return true;
    const auto& assumption = assumptions_->entries_.at(guard->assumption);
    if (!assumption.valid || assumption.epoch != guard->epoch) reason = "assumption invalidated";
    else if (force_guard == guard->id) reason = "forced guard failure";
    else if (args.at(guard->argument) != guard->expected) reason = "guard predicate failed";
    else return true;
    failed = &*guard;
    return false;
  });
  if (!failed) return {std::move(outcome), {}, {}, {}};
  counters_->deoptimizations.fetch_add(1, std::memory_order_relaxed);
  if (reason == "assumption invalidated") counters_->invalidations.fetch_add(1, std::memory_order_relaxed);
  else if (reason == "forced guard failure") counters_->forced.fetch_add(1, std::memory_order_relaxed);
  else counters_->predicate.fetch_add(1, std::memory_order_relaxed);
  ir::State recovered = std::move(outcome.state);
  recovered.block = failed->frame.block; recovered.offset = failed->frame.offset;
  std::map<ir::Id, std::uint32_t> locals;
  for (auto id : failed->frame.locals) locals.emplace(id, recovered.values.at(id));
  recovered.values = std::move(locals);
  auto result = ir::resume(ir::load(baseline_), args, std::move(recovered), budget);
  return {std::move(result), failed->id, failed->frame, std::move(reason)};
}
Statistics Version::statistics() const {
  return {counters_->executions.load(std::memory_order_relaxed), counters_->deoptimizations.load(std::memory_order_relaxed),
    counters_->predicate.load(std::memory_order_relaxed), counters_->invalidations.load(std::memory_order_relaxed),
    counters_->forced.load(std::memory_order_relaxed)};
}
} // namespace novostone::speculative
