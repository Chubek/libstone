#include "novostone/CCDB/CCDB.hpp"

#include <limits>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <locale>
#include <charconv>

namespace novostone::ccdb {
struct State {
  Revision revision = 0;
  std::shared_ptr<const datalog::Program> program;
  datalog::Facts facts;
  std::map<std::string, Revision> versions;
  mutable std::mutex mutex;
  mutable std::map<std::string, datalog::Result> cache;
};
struct Control {
  std::mutex mutex;
  std::shared_ptr<const State> current;
  Validator validator;
  Entity next = 1;
  std::vector<Event> events;
};
namespace {
std::set<std::string> changed(const State& before, const State& after) {
  std::set<std::string> result;
  for (const auto& [name, rows] : after.facts) if (rows != before.facts.at(name)) result.insert(name);
  return result;
}
void inherit_cache(const State& before, const State& after, const std::set<std::string>& changes) {
  std::scoped_lock lock(before.mutex, after.mutex);
  for (const auto& [name, cached] : before.cache) {
    bool valid = true;
    for (const auto& dependency : cached.dependencies) if (changes.count(dependency)) valid = false;
    if (valid) after.cache.emplace(name, cached);
  }
}
}
Revision Snapshot::revision() const { return state_->revision; }
const datalog::Facts& Snapshot::facts() const { return state_->facts; }
const datalog::Relation& Snapshot::relation(const std::string& name) const {
  if (state_->program->schemas().at(name).derived) throw std::invalid_argument("use query for derived relations");
  return state_->facts.at(name);
}
QueryResult Snapshot::query(const std::string& name, datalog::Limits limits) const {
  // Non-default budgets must be enforced, even when a previous query was cached.
  bool use_cache = limits.facts == datalog::Limits{}.facts && limits.work == datalog::Limits{}.work;
  if (use_cache) {
    std::lock_guard lock(state_->mutex);
    auto it = state_->cache.find(name);
    if (it != state_->cache.end()) return {it->second, revision(), true};
  }
  auto result = state_->program->evaluate(state_->facts, name, limits);
  if (use_cache) { std::lock_guard lock(state_->mutex); state_->cache.emplace(name, result); }
  return {std::move(result), revision(), false};
}
std::string Snapshot::dump() const {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "revision " << revision() << '\n';
  for (const auto& [name, rows] : facts()) out << name << ":\n" << datalog::dump(rows);
  return out.str();
}
Database::Database(datalog::Program program, Validator validator) : control_(std::make_shared<Control>()) {
  program.validate();
  auto state = std::make_shared<State>();
  state->program = std::make_shared<const datalog::Program>(std::move(program));
  for (const auto& [name, schema] : state->program->schemas()) if (!schema.derived) {
    state->facts[name]; state->versions[name] = 0;
  }
  control_->current = std::move(state);
  control_->validator = std::move(validator);
}
Snapshot Database::snapshot() const { std::lock_guard lock(control_->mutex); return Snapshot(control_->current); }
Transaction Database::begin() { return Transaction(control_, snapshot()); }
Transaction Database::begin(const Snapshot& expected) {
  std::lock_guard lock(control_->mutex);
  if (expected.state_ != control_->current) throw Conflict("stale or foreign snapshot");
  return Transaction(control_, expected);
}
Entity Database::allocate() {
  std::lock_guard lock(control_->mutex);
  if (control_->next == std::numeric_limits<Entity>::max()) throw std::overflow_error("entity IDs exhausted");
  return control_->next++;
}
std::vector<Event> Database::events() const { std::lock_guard lock(control_->mutex); return control_->events; }
std::string Database::checkpoint() const {
  std::shared_ptr<const State> state; Entity next;
  { std::lock_guard lock(control_->mutex); state = control_->current; next = control_->next; }
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "ccdb 1 " << state->revision << ' ' << next << '\n' << std::quoted(state->program->signature()) << '\n';
  for (const auto& [name, rows] : state->facts) for (const auto& row : rows) {
    out << "fact " << std::quoted(name);
    for (const auto& value : row) {
      if (const auto* integer = std::get_if<std::int64_t>(&value)) out << " i " << *integer;
      else out << " s " << std::quoted(std::get<std::string>(value));
    }
    out << '\n';
  }
  out << "end\n";
  return out.str();
}
Snapshot Database::restore(const Snapshot& expected, const std::string& text, RestoreLimits limits) {
  if (text.size() > limits.bytes) throw std::invalid_argument("checkpoint byte budget exceeded");
  auto transaction = begin(expected);
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  std::string token, signature; int version; Revision origin; Entity next;
  std::string revision;
  if (!(in >> token >> version >> revision >> next >> std::quoted(signature)) || token != "ccdb" || version != 1 || next <= 0)
    throw std::invalid_argument("invalid checkpoint header");
  auto parsed = std::from_chars(revision.data(), revision.data() + revision.size(), origin);
  if (parsed.ec != std::errc{} || parsed.ptr != revision.data() + revision.size()) throw std::invalid_argument("invalid checkpoint revision");
  if (signature != expected.state_->program->signature()) throw std::invalid_argument("checkpoint schema/rule identity mismatch");
  datalog::Facts facts;
  std::size_t count = 0;
  bool ended = false;
  while (in >> token) {
    if (token == "end") { ended = true; break; }
    std::string name;
    if (token != "fact" || !(in >> std::quoted(name))) throw std::invalid_argument("invalid checkpoint fact");
    if (++count > limits.facts) throw std::invalid_argument("checkpoint fact budget exceeded");
    const auto& schema = expected.state_->program->schemas().at(name);
    datalog::Tuple row;
    for (auto type : schema.columns) {
      std::string tag;
      if (!(in >> tag)) throw std::invalid_argument("truncated checkpoint fact");
      if (type == datalog::Type::Integer && tag == "i") {
        std::int64_t value;
        if (!(in >> value)) throw std::invalid_argument("invalid checkpoint integer");
        row.emplace_back(value);
      } else if (type == datalog::Type::Symbol && tag == "s") {
        std::string value;
        if (!(in >> std::quoted(value))) throw std::invalid_argument("invalid checkpoint symbol");
        row.emplace_back(std::move(value));
      } else throw std::invalid_argument("checkpoint column type mismatch");
    }
    expected.state_->program->validate_fact(name, row);
    if (!facts[name].insert(std::move(row)).second) throw std::invalid_argument("duplicate checkpoint fact");
  }
  if (!ended || (in >> token)) throw std::invalid_argument("truncated or trailing checkpoint data");
  for (const auto& [name, rows] : expected.facts()) for (const auto& row : rows) transaction.erase(name, row);
  for (const auto& [name, rows] : facts) for (const auto& row : rows) transaction.insert(name, row);
  if (next > 1) transaction.reserve(next - 1);
  return transaction.commit("restore checkpoint from revision " + std::to_string(origin));
}
Transaction::Transaction(std::shared_ptr<Control> control, Snapshot base)
    : control_(std::move(control)), base_(std::move(base)), facts_(base_.facts()) {}
void Transaction::active() const { if (!control_) throw std::logic_error("transaction is closed"); }
void Transaction::insert(const std::string& name, datalog::Tuple row) {
  active();
  base_.state_->program->validate_fact(name, row);
  writes_.insert(name);
  facts_.at(name).insert(std::move(row));
}
void Transaction::erase(const std::string& name, const datalog::Tuple& row) {
  active();
  base_.state_->program->validate_fact(name, row);
  writes_.insert(name);
  facts_.at(name).erase(row);
}
const datalog::Relation& Transaction::relation(const std::string& name) {
  active();
  if (base_.state_->program->schemas().at(name).derived) throw std::invalid_argument("use query for derived relations");
  reads_.insert(name);
  return facts_.at(name);
}
void Transaction::depend(const Snapshot& source, const std::set<std::string>& relations) {
  active();
  if (!source.same_revision(base_)) throw Conflict("read dependency has a stale or foreign revision");
  for (const auto& relation : relations) for (const auto& dependency : base_.state_->program->dependencies(relation))
    if (!base_.state_->program->schemas().at(dependency).derived) reads_.insert(dependency);
}
QueryResult Transaction::query(const std::string& name, datalog::Limits limits) {
  depend(base_, {name});
  return tentative().query(name, limits);
}
void Transaction::reserve(Entity through) {
  active();
  if (through <= 0) throw std::invalid_argument("entity IDs must be positive");
  std::lock_guard lock(control_->mutex);
  if (through >= control_->next)
    control_->next = through == std::numeric_limits<Entity>::max() ? through : through + 1;
}
Snapshot Transaction::preview() const {
  active();
  // An unrestricted snapshot exposes every primary relation to arbitrary code.
  for (const auto& [name, rows] : facts_) { (void)rows; reads_.insert(name); }
  return tentative();
}
Snapshot Transaction::tentative() const {
  active();
  if (base_.revision() == std::numeric_limits<Revision>::max()) throw std::overflow_error("revision IDs exhausted");
  auto state = std::make_shared<State>();
  state->revision = base_.revision() + 1;
  state->program = base_.state_->program;
  state->facts = facts_;
  state->versions = base_.state_->versions;
  for (const auto& name : changed(*base_.state_, *state)) state->versions[name] = state->revision;
  return Snapshot(std::move(state));
}
Snapshot Transaction::commit(std::string reason, Validator validator) {
  active();
  auto candidate = tentative();
  if (control_->validator) control_->validator(candidate);
  if (validator) validator(candidate);
  auto changes = changed(*base_.state_, *candidate.state_);
  inherit_cache(*base_.state_, *candidate.state_, changes);
  auto control = control_;
  std::lock_guard lock(control->mutex);
  if (control->current != base_.state_) throw Conflict("transaction revision conflict");
  control->events.push_back({candidate.revision(), std::move(reason), std::move(changes), base_.revision(), base_.revision(), reads_});
  control->current = candidate.state_;
  control_.reset();
  return candidate;
}
Snapshot Transaction::commit_disjoint(std::string reason, Validator validator, std::size_t retries) {
  active();
  auto control = control_;
  for (std::size_t attempt = 0;; ++attempt) {
    std::shared_ptr<const State> head;
    { std::lock_guard lock(control->mutex); head = control->current; }
    auto dependencies = reads_; dependencies.insert(writes_.begin(), writes_.end());
    for (const auto& name : dependencies) if (head->versions.at(name) != base_.state_->versions.at(name))
      throw Conflict("transaction dependency conflict: " + name);
    if (head->revision == std::numeric_limits<Revision>::max()) throw std::overflow_error("revision IDs exhausted");
    auto state = std::make_shared<State>();
    state->revision = head->revision + 1; state->program = head->program;
    state->facts = head->facts; state->versions = head->versions;
    for (const auto& name : writes_) state->facts.at(name) = facts_.at(name);
    auto changes = changed(*head, *state);
    for (const auto& name : changes) state->versions[name] = state->revision;
    Snapshot candidate(state);
    if (control->validator) control->validator(candidate);
    if (validator) validator(candidate);
    inherit_cache(*head, *state, changes);
    std::lock_guard lock(control->mutex);
    if (control->current != head) {
      if (attempt >= retries) throw Conflict("transaction rebase retry budget exhausted");
      continue;
    }
    control->events.push_back({state->revision, std::move(reason), std::move(changes), base_.revision(), head->revision, reads_});
    control->current = state; control_.reset();
    return candidate;
  }
}
void Transaction::abort() { control_.reset(); facts_.clear(); }
} // namespace novostone::ccdb
