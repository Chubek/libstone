#include "novostone/QueryBasedIR/QueryBasedIR.hpp"

#include <algorithm>
#include <sstream>
#include <tuple>

namespace novostone::query {
namespace {
void eligible(const ccdb::Snapshot& snapshot) {
  ir::verify(snapshot);
  if (!snapshot.relation("Guard").empty() || !snapshot.relation("Frame").empty() ||
      !snapshot.relation("Binding").empty() || !snapshot.relation("Assumption").empty())
    throw std::invalid_argument("guarded IR requires recovery-aware rewrites");
}
std::int64_t integer(const datalog::Value& value) { return std::get<std::int64_t>(value); }
ir::Instruction& find(ir::Function& f, ir::Id id) {
  for (auto& [block_id, block] : f.blocks) {
    (void)block_id;
    for (auto& i : block.instructions) if (i.id == id) return i;
  }
  throw std::logic_error("query refers to missing instruction");
}
void constant(ir::Instruction& i, std::uint32_t value) { i.op = ir::Op::Const; i.immediate = value; i.operands.clear(); i.targets.clear(); }
void copy(ir::Instruction& i, ir::Id value) { i.op = ir::Op::Copy; i.immediate = 0; i.operands = {value}; i.targets.clear(); }
void erase(ir::Function& f, ir::Id id) {
  for (auto& [block_id, block] : f.blocks) {
    (void)block_id;
    std::erase_if(block.instructions, [&](const auto& i) { return i.id == id; });
  }
}
void replace_uses(ir::Function& f, ir::Id id, ir::Id value) {
  for (auto& [block_id, block] : f.blocks) {
    (void)block_id;
    for (auto& user : block.instructions) for (auto& operand : user.operands) if (operand == id) operand = value;
  }
}
void trim_cfg(ir::Function& f) {
  std::set<ir::Id> reachable{f.entry}; std::vector<ir::Id> work{f.entry};
  while (!work.empty()) {
    auto id = work.back(); work.pop_back();
    for (auto target : f.blocks.at(id).instructions.back().targets)
      if (reachable.insert(target).second) work.push_back(target);
  }
  std::erase_if(f.blocks, [&](const auto& pair) { return !reachable.count(pair.first); });
  std::map<ir::Id, std::set<ir::Id>> predecessors;
  for (const auto& [id, block] : f.blocks)
    for (auto target : block.instructions.back().targets) predecessors[target].insert(id);
  for (auto& [id, block] : f.blocks) for (auto& phi : block.instructions) {
    if (phi.op != ir::Op::Phi) break;
    for (std::size_t slot = phi.targets.size(); slot-- > 0;) if (!predecessors[id].count(phi.targets[slot])) {
      phi.targets.erase(phi.targets.begin() + static_cast<std::ptrdiff_t>(slot));
      phi.operands.erase(phi.operands.begin() + static_cast<std::ptrdiff_t>(slot));
    }
  }
}
struct Budget {
  Options options;
  std::size_t work = 0;
  std::size_t analysis_work = 0;
  ccdb::QueryResult read(const ccdb::Snapshot& snapshot, const std::string& name) {
    if (work >= options.query_work) throw datalog::BudgetExceeded("optimizer query work exhausted");
    try {
      auto result = snapshot.query(name, {options.query_facts, options.query_work - work});
      // Account conservatively even if CCDB supplied a cached analysis.
      work += result.work;
      return result;
    } catch (const datalog::BudgetExceeded&) {
      // A failed query has no result/work counter; charge its full reservation.
      work = options.query_work;
      throw;
    }
  }
};
std::optional<Rewrite> step(const ccdb::Snapshot& snapshot, ir::Function& f, Budget& budget) {
  using ir::Op;
  auto constants = budget.read(snapshot, "Constant");
  std::map<ir::Id, std::uint32_t> known;
  for (const auto& row : constants.rows) known.emplace(integer(row[0]), static_cast<std::uint32_t>(integer(row[1])));
  auto branches = budget.read(snapshot, "BranchValue");
  for (const auto& row : branches.rows) {
    auto& branch = find(f, integer(row[0]));
    auto condition = known.find(integer(row[1]));
    if (condition == known.end() && row[2] != row[3]) continue;
    auto target = integer(row[condition == known.end() || condition->second ? 2 : 3]);
    auto id = branch.id;
    branch.op = Op::Jump; branch.operands.clear(); branch.targets = {target};
    trim_cfg(f);
    return Rewrite{snapshot.revision(), id, "fold_branch", {"Instr", "Operand", "Target", "Block", "Entry"}};
  }
  for (const auto& [block_id, block] : f.blocks) {
    (void)block_id;
    for (const auto& phi : block.instructions) {
      if (phi.op != Op::Phi) break;
      auto value = phi.operands.front();
      if (value != phi.id && std::all_of(phi.operands.begin(), phi.operands.end(), [&](auto operand) { return operand == value; })) {
        auto id = phi.id; replace_uses(f, id, value); erase(f, id);
        return Rewrite{snapshot.revision(), id, "phi_uniform", {"Instr", "Operand", "Target"}};
      }
    }
  }
  auto folds = budget.read(snapshot, "Fold");
  if (!folds.rows.empty()) {
    const auto& row = *folds.rows.begin();
    auto& i = find(f, integer(row[0]));
    auto a = static_cast<std::uint32_t>(integer(row[2])), b = static_cast<std::uint32_t>(integer(row[3]));
    std::uint32_t value;
    switch (i.op) {
      case Op::Add: value = static_cast<std::uint32_t>(std::uint64_t{a} + b); break;
      case Op::Sub: value = static_cast<std::uint32_t>(std::uint64_t{a} - b); break;
      case Op::Mul: value = static_cast<std::uint32_t>(std::uint64_t{a} * b); break;
      case Op::Eq: value = a == b; break;
      case Op::Ult: value = a < b; break;
      default: throw std::logic_error("invalid binary query");
    }
    auto rule = "fold_" + ir::name(i.op); constant(i, value);
    return Rewrite{snapshot.revision(), i.id, rule, folds.dependencies};
  }
  for (const auto& relation : {"ConstCandidate", "CopyCandidate"}) {
    auto matches = budget.read(snapshot, relation);
    if (matches.rows.empty()) continue;
    const auto& row = *matches.rows.begin();
    auto& i = find(f, integer(row[0]));
    if (std::string(relation) == "ConstCandidate") constant(i, static_cast<std::uint32_t>(integer(row[2])));
    else copy(i, integer(row[2]));
    return Rewrite{snapshot.revision(), i.id, std::get<std::string>(row[1]), matches.dependencies};
  }
  auto copies = budget.read(snapshot, "CopyValue");
  if (!copies.rows.empty()) {
    const auto& row = *copies.rows.begin();
    auto id = integer(row[0]), value = integer(row[1]);
    replace_uses(f, id, value);
    erase(f, id);
    return Rewrite{snapshot.revision(), id, "copy_propagation", copies.dependencies};
  }
  // Value numbering is local to a block. Phi values are edge-dependent and
  // cannot be treated as ordinary expressions evaluated at an instruction PC.
  for (auto& [block_id, block] : f.blocks) {
    (void)block_id;
    std::map<std::tuple<Op, std::uint32_t, std::vector<ir::Id>>, ir::Id> expressions;
    struct Access { ir::Id address; ir::Id value; };
    std::vector<Access> available;
    auto same = [&](ir::Id a, ir::Id b) { return a == b || (known.count(a) && known.count(b) && known.at(a) == known.at(b)); };
    auto disjoint = [&](ir::Id a, ir::Id b) { return known.count(a) && known.count(b) && known.at(a) != known.at(b); };
    for (auto& i : block.instructions) {
      if (i.op == Op::Store) {
        auto address = i.operands[0];
        std::erase_if(available, [&](const auto& access) { return !disjoint(address, access.address); });
        available.push_back({address, i.operands[1]});
      } else if (i.op == Op::Load) {
        auto it = std::find_if(available.rbegin(), available.rend(), [&](const auto& access) { return same(i.operands[0], access.address); });
        if (it != available.rend()) {
          // A preceding successful access proves this same address in bounds;
          // no allocation, resize, volatile, or concurrent memory exists here.
          copy(i, it->value);
          return Rewrite{snapshot.revision(), i.id, "forward_load", {"Instr", "Operand"}};
        }
        available.push_back({i.operands[0], i.id});
      } else if (ir::pure(i.op) && i.op != Op::Phi) {
        auto operands = i.operands;
        if (i.op == Op::Add || i.op == Op::Mul || i.op == Op::Eq) std::sort(operands.begin(), operands.end());
        auto [it, inserted] = expressions.emplace(std::make_tuple(i.op, i.immediate, std::move(operands)), i.id);
        if (!inserted) {
          copy(i, it->second);
          return Rewrite{snapshot.revision(), i.id, "local_cse", {"Instr", "Operand"}};
        }
      }
    }
  }
  auto dead = budget.read(snapshot, "Dead");
  if (!dead.rows.empty()) {
    auto id = integer(dead.rows.begin()->at(0)); erase(f, id);
    return Rewrite{snapshot.revision(), id, "dead_pure", dead.dependencies};
  }
  auto dead_group = budget.read(snapshot, "DeadGroup");
  if (!dead_group.rows.empty()) {
    auto id = integer(dead_group.rows.begin()->at(0));
    for (const auto& row : dead_group.rows) erase(f, integer(row[0]));
    return Rewrite{snapshot.revision(), id, "dead_pure_cycle", dead_group.dependencies};
  }
  if (budget.options.conditional_constants) {
    Propagation analysis;
    try {
      analysis = propagate(f, budget.options.analysis_work - budget.analysis_work);
      budget.analysis_work += analysis.work;
    } catch (const datalog::BudgetExceeded&) {
      budget.analysis_work = budget.options.analysis_work; throw;
    }
    for (auto& [block_id, block] : f.blocks) {
      if (!analysis.blocks.count(block_id)) continue;
      for (std::size_t slot = 0; slot < block.instructions.size(); ++slot) {
        auto& i = block.instructions[slot];
        if (!ir::produces_value(i.op) || i.op == Op::Const || !ir::pure(i.op)) continue;
        auto value = analysis.values.at(i.id);
        if (value.kind != ValueKind::Constant) continue;
        auto id = i.id; bool phi = i.op == Op::Phi;
        constant(i, value.value);
        if (phi) {
          auto replacement = i;
          block.instructions.erase(block.instructions.begin() + static_cast<std::ptrdiff_t>(slot));
          auto first = std::find_if(block.instructions.begin(), block.instructions.end(), [](const auto& next) { return next.op != Op::Phi; });
          block.instructions.insert(first, std::move(replacement));
        }
        return Rewrite{snapshot.revision(), id, "conditional_constant", {"Signature", "Block", "Entry", "Instr", "Operand", "Target"}};
      }
    }
  }
  return std::nullopt;
}
}
Candidate baseline(const ccdb::Snapshot& snapshot) { eligible(snapshot); return {snapshot, snapshot, {}, false}; }
Candidate optimize(const ccdb::Snapshot& snapshot, std::size_t budget) {
  Options options; options.rewrites = budget;
  return optimize(snapshot, options);
}
Candidate optimize(const ccdb::Snapshot& snapshot, Options options) {
  eligible(snapshot);
  ccdb::Database scratch(ir::program(), [](const auto& s) { ir::verify(s); });
  auto transaction = scratch.begin();
  ir::store(transaction, ir::load(snapshot));
  auto current = transaction.commit("optimization input");
  std::vector<Rewrite> rewrites;
  Budget budget{options};
  try {
    for (std::size_t n = 0; n < options.rewrites; ++n) {
      auto f = ir::load(current);
      auto rewrite = step(current, f, budget);
      if (!rewrite) return {snapshot, current, std::move(rewrites), false, {}, budget.work, budget.analysis_work};
      current = ir::publish(scratch, current, f, rewrite->rule);
      rewrites.push_back(std::move(*rewrite));
    }
    auto f = ir::load(current);
    bool exhausted = step(current, f, budget).has_value();
    return {snapshot, current, std::move(rewrites), exhausted, exhausted ? "rewrite limit reached" : "", budget.work, budget.analysis_work};
  } catch (const datalog::BudgetExceeded& error) {
    // Tentative work is private. Return the last committed, verified prefix.
    return {snapshot, current, std::move(rewrites), true, error.what(), budget.work, budget.analysis_work};
  }
}
ccdb::Snapshot apply(ccdb::Database& database, const Candidate& candidate) {
  return ir::publish(database, candidate.base(), ir::load(candidate.result()), candidate.explain());
}
std::string Candidate::explain() const {
  std::ostringstream out;
  out << "source revision " << base_.revision() << "; " << rewrites_.size() << " rewrites";
  for (const auto& rewrite : rewrites_) {
    out << "\nrevision " << rewrite.revision << " %" << rewrite.instruction << ' ' << rewrite.rule << " reads";
    for (const auto& dependency : rewrite.dependencies) out << ' ' << dependency;
  }
  out << "\nquery work " << work_;
  out << "\nanalysis work " << analysis_work_;
  if (exhausted_) out << "\noptimization budget exhausted: " << stop_reason_;
  return out.str();
}
} // namespace novostone::query
