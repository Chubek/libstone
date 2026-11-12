#include "novostone/Datalog/Datalog.hpp"

#include <algorithm>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <locale>

namespace novostone::datalog {
namespace {
Type type(const Value& v) { return std::holds_alternative<std::int64_t>(v) ? Type::Integer : Type::Symbol; }
using Bindings = std::map<std::string, Value>;
bool match(const Atom& atom, const Tuple& row, Bindings& bindings) {
  for (std::size_t i = 0; i < row.size(); ++i) {
    const auto& t = atom.terms[i];
    if (t.kind == Term::Kind::Constant && t.value != row[i]) return false;
    if (t.kind == Term::Kind::Variable) {
      auto [it, inserted] = bindings.emplace(std::get<std::string>(t.value), row[i]);
      if (!inserted && it->second != row[i]) return false;
    }
  }
  return true;
}
}
Term Term::var(std::string name) {
  if (name.empty()) throw std::invalid_argument("empty variable name");
  return {Kind::Variable, std::move(name)};
}
Term Term::constant(Value value) { return {Kind::Constant, std::move(value)}; }
Term Term::any() { return {Kind::Wildcard, std::int64_t{0}}; }
void Program::declare(std::string name, std::vector<Type> columns, bool derived) {
  for (auto column : columns) if (column != Type::Integer && column != Type::Symbol)
    throw std::invalid_argument("invalid relation column type");
  if (name.empty() || !schemas_.emplace(name, Schema{std::move(columns), derived}).second)
    throw std::invalid_argument("duplicate or empty relation: " + name);
}
void Program::add(Rule rule) { rules_.push_back(std::move(rule)); }
std::map<std::string, std::size_t> Program::strata() const {
  std::map<std::string, std::size_t> levels;
  for (const auto& [name, schema] : schemas_) { (void)schema; levels[name] = 0; }
  for (std::size_t iteration = 0; iteration <= schemas_.size(); ++iteration) {
    bool changed = false;
    for (const auto& rule : rules_) for (const auto& atom : rule.body) {
      auto required = levels.at(atom.relation) + (atom.negated ? 1 : 0);
      auto& level = levels.at(rule.head.relation);
      if (required > level) { level = required; changed = true; }
    }
    if (!changed) return levels;
  }
  throw std::invalid_argument("Datalog recursion through negation");
}
void Program::validate() const {
  for (const auto& rule : rules_) {
    if (rule.head.negated || !schemas_.at(rule.head.relation).derived)
      throw std::invalid_argument("rule head must be a positive derived relation");
    std::map<std::string, Type> variables;
    std::set<std::string> bound;
    auto check = [&](const Atom& atom, bool head) {
      const auto& schema = schemas_.at(atom.relation);
      if (schema.columns.size() != atom.terms.size()) throw std::invalid_argument("rule arity mismatch");
      for (std::size_t i = 0; i < atom.terms.size(); ++i) {
        const auto& t = atom.terms[i];
        if (t.kind != Term::Kind::Variable && t.kind != Term::Kind::Constant && t.kind != Term::Kind::Wildcard)
          throw std::invalid_argument("invalid term kind");
        if (t.kind == Term::Kind::Wildcard && head) throw std::invalid_argument("wildcard in rule head");
        if (t.kind == Term::Kind::Constant && type(t.value) != schema.columns[i])
          throw std::invalid_argument("rule constant type mismatch");
        if (t.kind == Term::Kind::Variable) {
          if (!std::holds_alternative<std::string>(t.value) || std::get<std::string>(t.value).empty())
            throw std::invalid_argument("invalid variable name");
          const auto& name = std::get<std::string>(t.value);
          auto [it, inserted] = variables.emplace(name, schema.columns[i]);
          if (!inserted && it->second != schema.columns[i]) throw std::invalid_argument("variable type mismatch");
          if (!head && !atom.negated) bound.insert(name);
        }
      }
    };
    check(rule.head, true);
    for (const auto& atom : rule.body) check(atom, false);
    auto safe = [&](const Atom& atom) {
      for (const auto& t : atom.terms)
        if (t.kind == Term::Kind::Variable && !bound.count(std::get<std::string>(t.value)))
          throw std::invalid_argument("unbound variable in head or negation");
    };
    safe(rule.head);
    for (const auto& atom : rule.body) if (atom.negated) safe(atom);
  }
  (void)strata();
}
std::string Program::signature() const {
  validate();
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "datalog 1 " << schemas_.size() << '\n';
  for (const auto& [name, schema] : schemas_) {
    out << std::quoted(name) << ' ' << schema.derived << ' ' << schema.columns.size();
    for (auto column : schema.columns) out << ' ' << static_cast<int>(column);
    out << '\n';
  }
  auto atom = [&](const Atom& a) {
    out << std::quoted(a.relation) << ' ' << a.negated << ' ' << a.terms.size();
    for (const auto& t : a.terms) {
      out << ' ' << static_cast<int>(t.kind) << ' ';
      if (t.kind == Term::Kind::Wildcard) out << '_';
      else if (auto integer = std::get_if<std::int64_t>(&t.value)) out << "i " << *integer;
      else out << "s " << std::quoted(std::get<std::string>(t.value));
    }
    out << '\n';
  };
  out << rules_.size() << '\n';
  for (const auto& rule : rules_) {
    atom(rule.head); out << rule.body.size() << '\n';
    for (const auto& a : rule.body) atom(a);
  }
  return out.str();
}
void Program::validate_fact(const std::string& name, const Tuple& row) const {
  const auto& schema = schemas_.at(name);
  if (schema.derived) throw std::invalid_argument("cannot write derived relation: " + name);
  if (row.size() != schema.columns.size()) throw std::invalid_argument("fact arity mismatch: " + name);
  for (std::size_t i = 0; i < row.size(); ++i)
    if (type(row[i]) != schema.columns[i]) throw std::invalid_argument("fact type mismatch: " + name);
}
std::set<std::string> Program::dependencies(const std::string& goal) const {
  (void)schemas_.at(goal);
  std::set<std::string> result{goal};
  bool changed;
  do {
    changed = false;
    for (const auto& rule : rules_) if (result.count(rule.head.relation))
      for (const auto& atom : rule.body) changed |= result.insert(atom.relation).second;
  } while (changed);
  return result;
}
Result Program::evaluate(const Facts& facts, const std::string& goal, Limits limits, Evaluation evaluation) const {
  validate();
  if (evaluation != Evaluation::SemiNaive && evaluation != Evaluation::Naive) throw std::invalid_argument("invalid evaluation strategy");
  Result result;
  auto needed = dependencies(goal);
  Facts tables;
  std::size_t count = 0;
  auto tick = [&] {
    if (result.work >= limits.work) throw BudgetExceeded("Datalog work budget exceeded");
    ++result.work;
  };
  for (const auto& name : needed) {
    tables[name];
    if (!schemas_.at(name).derived) result.dependencies.insert(name);
  }
  for (const auto& [name, rows] : facts) {
    if (schemas_.at(name).derived) throw std::invalid_argument("cannot supply derived facts: " + name);
    for (const auto& row : rows) { tick(); validate_fact(name, row); }
    if (needed.count(name)) { tables[name] = rows; count += rows.size(); }
  }
  if (count > limits.facts) throw BudgetExceeded("Datalog fact budget exceeded");
  auto levels = strata();
  std::size_t max_level = 0;
  for (const auto& name : needed) max_level = std::max(max_level, levels.at(name));
  using Index = std::vector<std::map<Value, std::vector<const Tuple*>>>;
  std::map<std::string, Index> full_indices, delta_indices;
  auto lookup = [&](const Atom& atom, const Bindings& bindings, const Relation& table,
                    std::map<std::string, Index>& indices) -> const std::vector<const Tuple*>* {
    const std::vector<const Tuple*>* best = nullptr;
    for (std::size_t column = 0; column < atom.terms.size(); ++column) {
      const auto& term = atom.terms[column];
      const Value* key = nullptr;
      if (term.kind == Term::Kind::Constant) key = &term.value;
      else if (term.kind == Term::Kind::Variable) {
        auto it = bindings.find(std::get<std::string>(term.value));
        if (it != bindings.end()) key = &it->second;
      }
      if (!key) continue;
      auto [it, inserted] = indices.try_emplace(atom.relation);
      if (inserted) {
        ++result.statistics.index_builds;
        it->second.resize(atom.terms.size());
        for (const auto& row : table) for (std::size_t c = 0; c < row.size(); ++c) {
          tick(); it->second[c][row[c]].push_back(&row);
        }
      }
      const auto& rows = it->second[column][*key];
      if (!best || rows.size() < best->size()) best = &rows;
    }
    return best;
  };
  struct Plan {
    const Rule* rule;
    std::vector<const Atom*> body;
    std::vector<std::size_t> recursive;
  };
  for (std::size_t level = 0; level <= max_level; ++level) {
    std::vector<Plan> plans;
    for (const auto& rule : rules_) {
      if (!needed.count(rule.head.relation) || levels.at(rule.head.relation) != level) continue;
      if (rule.body.size() > 256) throw BudgetExceeded("Datalog join depth exceeded");
      Plan plan{&rule, {}, {}};
      for (const auto& atom : rule.body) if (!atom.negated) {
        if (schemas_.at(atom.relation).derived && levels.at(atom.relation) == level) plan.recursive.push_back(plan.body.size());
        plan.body.push_back(&atom);
      }
      for (const auto& atom : rule.body) if (atom.negated) plan.body.push_back(&atom);
      plans.push_back(std::move(plan));
    }
    if (plans.empty()) continue;
    Facts delta;
    bool first = true;
    do {
      ++result.statistics.rounds;
      Facts pending;
      std::size_t pending_count = 0;
      delta_indices.clear();
      auto run = [&](const Plan& plan, std::size_t pivot) {
        ++result.statistics.rule_evaluations;
        std::function<void(std::size_t, const Bindings&)> join = [&](std::size_t index, const Bindings& bindings) {
          tick();
          if (index == plan.body.size()) {
            Tuple row;
            for (const auto& t : plan.rule->head.terms)
              row.push_back(t.kind == Term::Kind::Constant ? t.value : bindings.at(std::get<std::string>(t.value)));
            const auto& head = plan.rule->head.relation;
            if (!tables.at(head).count(row) && pending[head].insert(std::move(row)).second) {
              if (pending_count >= limits.facts - count) throw BudgetExceeded("Datalog fact budget exceeded");
              ++pending_count;
            }
            return;
          }
          const auto& atom = *plan.body[index];
          const auto& table = index == pivot ? delta.at(atom.relation) : tables.at(atom.relation);
          auto& indices = index == pivot ? delta_indices : full_indices;
          bool found = false;
          auto visit = [&](const Tuple& row) {
            tick(); ++result.statistics.tuple_visits;
            auto next = bindings;
            if (!match(atom, row, next)) return;
            found = true;
            if (!atom.negated) join(index + 1, next);
          };
          if (auto indexed = lookup(atom, bindings, table, indices)) {
            for (const auto* row : *indexed) { visit(*row); if (atom.negated && found) break; }
          } else {
            for (const auto& row : table) { visit(row); if (atom.negated && found) break; }
          }
          if (atom.negated && !found) join(index + 1, bindings);
        };
        join(0, {});
      };
      for (const auto& plan : plans) {
        if (evaluation == Evaluation::Naive || (first && plan.recursive.empty())) run(plan, plan.body.size());
        else if (!first) for (auto pivot : plan.recursive) {
          auto it = delta.find(plan.body[pivot]->relation);
          if (it != delta.end() && !it->second.empty()) run(plan, pivot);
        }
      }
      for (const auto& [name, rows] : pending) {
        full_indices.erase(name);
        tables.at(name).insert(rows.begin(), rows.end());
      }
      count += pending_count;
      delta = std::move(pending);
      first = false;
    } while (!delta.empty());
  }
  result.rows = std::move(tables.at(goal));
  return result;
}
std::string dump(const Relation& rows) {
  std::ostringstream out;
  for (const auto& row : rows) {
    out << '(';
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i) out << ", ";
      if (auto integer = std::get_if<std::int64_t>(&row[i])) out << *integer;
      else out << std::quoted(std::get<std::string>(row[i]));
    }
    out << ")\n";
  }
  return out.str();
}
} // namespace novostone::datalog
