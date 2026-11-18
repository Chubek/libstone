// Private implementation of Unisel's exact-cover solver. Keep Satie types out
// of the graph, public model, and installed headers.
#include "unisel.hpp"
#include <SatieCDCL.hpp>
#include <SatieTheoryUtils.hpp>
#include <bit>
#include <limits>
#include <map>

namespace limestone::unisel::detail {
namespace {
bool selected(const satie::Assignment& witness, satie::Lit literal) {
  return witness.get_literal(literal) == satie::Value::TRUE;
}

// Project source dependencies onto the cover, just as Scheduler IR emission
// does. Find an actual cycle, not the whole set of nodes left after a topo sort:
// removing a downstream node cannot break the cycle.
satie::Clause cyclic_cover(const ConstraintModel& model,
                          std::span<const schedrow::Dependency> dependencies,
                          const satie::Assignment& witness) {
  using Index = satie::Var;
  std::map<NodeId, Index> owner;
  std::map<Index, std::vector<Index>> edges;
  for (size_t i = 0; i < model.candidates.size(); ++i) {
    auto index = static_cast<Index>(i + 1);
    if (!selected(witness, index)) continue;
    edges.try_emplace(index);
    for (auto id : model.candidates[i].covered) owner.emplace(id, index);
  }
  auto add_edge = [&](Index from, Index to) {
    if (from != to) edges.at(from).push_back(to);
  };
  for (const auto& [index, _] : edges) {
    for (auto input : model.candidates[index - 1].inputs)
      if (auto from = owner.find(input); from != owner.end()) add_edge(from->second, index);
  }
  for (const auto& dep : dependencies) add_edge(owner.at(dep.producer), owner.at(dep.consumer));
  for (auto& [_, children] : edges) {
    std::sort(children.begin(), children.end());
    children.erase(std::unique(children.begin(), children.end()), children.end());
  }

  enum class Mark { Unvisited, Visiting, Done };
  std::map<Index, Mark> marks;
  for (const auto& [root, _] : edges) {
    if (marks[root] != Mark::Unvisited) continue;
    marks[root] = Mark::Visiting;
    std::vector<std::pair<Index, size_t>> path{{root, 0}};
    while (!path.empty()) {
      auto& [node, cursor] = path.back();
      const auto& children = edges.at(node);
      if (cursor == children.size()) {
        marks[node] = Mark::Done;
        path.pop_back();
        continue;
      }
      auto child = children[cursor++];
      if (marks[child] == Mark::Unvisited) {
        marks[child] = Mark::Visiting;
        path.emplace_back(child, 0);
      } else if (marks[child] == Mark::Visiting) {
        satie::Clause clause;
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
          clause.push_back(-it->first);
          if (it->first == child) break;
        }
        return clause;
      }
    }
  }
  return {};
}

Result<Selection> optimize_cover(const ConstraintModel& model,
                                 std::span<const schedrow::Dependency> dependencies) {
  using Answer = Result<Selection>;
  using Probe = Result<std::optional<satie::Assignment>>;
  const auto count = model.candidates.size();
  if (std::any_of(model.clauses.begin(), model.clauses.end(),
                  [](const auto& clause) { return clause.empty(); }))
    return Answer::err({Error::Code::Unsatisfiable, "required operation has no covering candidate"});

  uint64_t total = 0;
  size_t positive = 0;
  for (const auto& candidate : model.candidates) {
    total += static_cast<uint64_t>(candidate.cost);
    positive += candidate.cost != 0;
  }
  const size_t width = std::bit_width(total);
  // Satie CDCL uses signed int watch/clauses indices. Each ripple bit adds
  // three variables and thirteen clauses; leave headroom for learned clauses.
  constexpr uint64_t limit = std::numeric_limits<int32_t>::max() / 2;
  const auto adders = positive ? static_cast<uint64_t>(positive - 1) * width : 0;
  if (count + 1 + 3 * adders >= limit ||
      model.clauses.size() + 1 + 13 * adders + count + width >= limit)
    return Answer::err({Error::Code::ResourceLimit, "selection objective exceeds Satie index limits"});

  satie::CNF formula(model.clauses);
  formula.set_declared_variable_count(count);
  std::vector<satie::Lit> cost_bits;
  if (width) {
    // Bit-blast once using Satie's theory encoder. A balanced adder tree
    // keeps the path through the weighted sum logarithmic in term count.
    satie::theory::Encoder encoder(static_cast<satie::Var>(count + 1));
    auto zero = encoder.const_false();
    std::vector<std::vector<satie::Lit>> level;
    for (size_t i = 0; i < count; ++i) {
      auto cost = static_cast<uint64_t>(model.candidates[i].cost);
      if (!cost) continue;
      std::vector<satie::Lit> term(width, zero);
      for (size_t bit = 0; bit < width; ++bit)
        if ((cost >> bit) & 1) term[bit] = static_cast<satie::Lit>(i + 1);
      level.push_back(std::move(term));
    }
    while (level.size() > 1) {
      std::vector<std::vector<satie::Lit>> next;
      next.reserve((level.size() + 1) / 2);
      for (size_t i = 0; i + 1 < level.size(); i += 2) {
        auto sum = encoder.ripple_add(level[i], level[i + 1], zero);
        sum.pop_back(); // All nonnegative partial sums fit the common width.
        next.push_back(std::move(sum));
      }
      if (level.size() % 2) next.push_back(std::move(level.back()));
      level = std::move(next);
    }
    cost_bits = std::move(level.front());
    for (auto& clause : encoder.take_clauses()) formula.add_clause(std::move(clause));
  }

  auto commit = [&](satie::Clause clause) {
    formula.add_clause(std::move(clause));
  };
  auto find = [&](satie::Lit trial_literal) -> Probe {
    for (;;) {
      if (formula.clause_count() >= limit)
        return Probe::err({Error::Code::ResourceLimit, "selection formula exceeds Satie clause index limits"});
      auto trial = formula;
      // Satie's current solve_under can repeatedly reassert a conflicting
      // assumption. A temporary root-level unit avoids that path, and the
      // shared weighted sum circuit is still encoded only once.
      if (trial_literal) trial.add_clause({trial_literal});
      auto answer = satie::solve_cdcl(trial);
      switch (answer.status) {
      case satie::SolveStatus::UNSAT: return Probe::ok(std::nullopt);
      case satie::SolveStatus::SAT: break;
      default: return Probe::err({Error::Code::Internal, "Satie could not decide the selection formula"});
      }
      if (!satie::is_formula_satisfied(trial, answer.assignment))
        return Probe::err({Error::Code::Internal, "Satie returned an invalid selection witness"});
      // Without explicit edges, all inter-candidate edges start at a selected
      // root and follow source SSA paths to other roots; they cannot cycle.
      auto cycle = dependencies.empty() ? satie::Clause{} :
                   cyclic_cover(model, dependencies, answer.assignment);
      if (cycle.empty()) return Probe::ok(std::move(answer.assignment));
      // Every edge in this cycle depends only on the listed candidates.
      // Taking all of them is illegal under any objective or prefix choice.
      commit(std::move(cycle));
    }
  };
  auto initial = find(0);
  if (!initial) return Answer::err(initial.error());
  if (!initial.value())
    return Answer::err({Error::Code::Unsatisfiable, "no acyclic exact instruction covering satisfies the selection model"});
  auto witness = std::move(*initial.value());
  auto prefer = [&](satie::Lit literal) -> Result<int> {
    if (!selected(witness, literal)) {
      auto better = find(literal);
      if (!better) return Result<int>::err(better.error());
      if (better.value()) witness = std::move(*better.value());
    }
    commit({selected(witness, literal) ? literal : -literal});
    return Result<int>::ok(0);
  };
  // Lexicographically minimize the unsigned sum, most significant bit first,
  // then prefer selected candidates in the model's stable order.
  for (auto bit = cost_bits.rbegin(); bit != cost_bits.rend(); ++bit) {
    auto fixed = prefer(-*bit);
    if (!fixed) return Answer::err(fixed.error());
  }
  uint64_t optimum = 0;
  for (size_t bit = 0; bit < cost_bits.size(); ++bit)
    if (selected(witness, cost_bits[bit])) optimum |= uint64_t{1} << bit;
  if (optimum > std::numeric_limits<int>::max())
    return Answer::err({Error::Code::ResourceLimit, "selection cost overflow"});
  Selection selection;
  uint64_t decoded = 0;
  for (size_t i = 0; i < count; ++i) {
    auto literal = static_cast<satie::Lit>(i + 1);
    auto fixed = prefer(literal);
    if (!fixed) return Answer::err(fixed.error());
    if (selected(witness, literal)) {
      selection.selected.push_back(model.candidates[i]);
      decoded += static_cast<uint64_t>(model.candidates[i].cost);
    }
  }
  if (decoded != optimum)
    return Answer::err({Error::Code::Internal, "Satie selection does not match its weighted objective"});
  selection.cost = static_cast<int>(decoded);
  return Answer::ok(std::move(selection));
}
} // namespace
} // namespace limestone::unisel::detail
