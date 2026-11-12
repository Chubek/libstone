#include "novostone/DatabaseBasedIR/DatabaseBasedIR.hpp"

#include <sstream>
#include <locale>

namespace novostone::ir {
std::string Liveness::explain() const {
  std::ostringstream out; out.imbue(std::locale::classic());
  out << "liveness revision " << revision << '\n';
  auto values = [&](const std::set<Id>& set) { for (auto value : set) out << " %" << value; };
  for (const auto& [block, in] : live_in) {
    out << "block " << block << " in"; values(in); out << " out"; values(live_out.at(block)); out << '\n';
  }
  for (const auto& [edge, live] : edges) {
    out << "edge " << edge.first << " -> " << edge.second; values(live); out << '\n';
  }
  for (const auto& [instruction, live] : before) {
    out << '%' << instruction << " before"; values(live); out << " after"; values(after.at(instruction)); out << '\n';
  }
  return out.str();
}
Liveness liveness(const ccdb::Snapshot& snapshot, datalog::Limits limits) {
  auto function = load(snapshot); verify(function);
  Liveness result; result.revision = snapshot.revision();
  auto number = [](const datalog::Value& value) { return std::get<std::int64_t>(value); };
  for (const auto& row : snapshot.query("LiveIn", limits).rows) result.live_in[number(row[0])].insert(number(row[1]));
  for (const auto& row : snapshot.query("LiveOut", limits).rows) result.live_out[number(row[0])].insert(number(row[1]));
  for (const auto& row : snapshot.query("LiveEdge", limits).rows) result.edges[{number(row[0]), number(row[1])}].insert(number(row[2]));
  std::map<Id, std::set<Id>> uses;
  for (const auto& row : snapshot.query("UseAt", limits).rows) uses[number(row[0])].insert(number(row[1]));
  for (const auto& [id, block] : function.blocks) {
    result.live_in[id]; auto live = result.live_out[id];
    for (auto target : block.instructions.back().targets) result.edges[{id, target}];
    for (auto it = block.instructions.rbegin(); it != block.instructions.rend(); ++it) {
      if (it->op == Op::Phi) break;
      result.after[it->id] = live;
      if (produces_value(it->op)) live.erase(it->id);
      live.insert(uses[it->id].begin(), uses[it->id].end());
      result.before[it->id] = live;
    }
    for (const auto& phi : block.instructions) {
      if (phi.op != Op::Phi) break;
      result.before[phi.id] = result.live_in[id]; result.after[phi.id] = live;
    }
  }
  return result;
}
Liveness liveness(const Function& function, datalog::Limits limits) {
  ccdb::Database database(program());
  auto snapshot = publish(database, database.snapshot(), function, "liveness input");
  return liveness(snapshot, limits);
}
std::vector<Loop> natural_loops(const Function& function) {
  verify(function);
  auto dom = dominators(function);
  std::map<Id, std::set<Id>> predecessors;
  for (const auto& [id, block] : function.blocks)
    for (auto target : block.instructions.back().targets) predecessors[target].insert(id);
  std::map<Id, Loop> loops;
  for (const auto& [latch, block] : function.blocks) for (auto header : block.instructions.back().targets) {
    if (!dom.at(latch).count(header)) continue;
    auto [it, inserted] = loops.try_emplace(header, Loop{header, {}, {header}, {}}); (void)inserted;
    auto& loop = it->second;
    loop.latches.insert(latch);
    std::vector<Id> work;
    if (loop.blocks.insert(latch).second) work.push_back(latch);
    while (!work.empty()) {
      auto id = work.back(); work.pop_back();
      for (auto predecessor : predecessors[id]) if (loop.blocks.insert(predecessor).second) work.push_back(predecessor);
    }
  }
  std::vector<Loop> result;
  for (auto& [header, loop] : loops) {
    (void)header;
    for (auto id : loop.blocks) for (auto target : function.blocks.at(id).instructions.back().targets)
      if (!loop.blocks.count(target)) loop.exits.insert({id, target});
    result.push_back(std::move(loop));
  }
  return result;
}
} // namespace novostone::ir
