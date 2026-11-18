#include <rtmbin/rtmbin.hpp>

#include "utils/Graph.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace rtmbin {

Result<GraphSummary> summarize_graph(BlockId entry, std::span<const Block> blocks) {
  if (blocks.size() > max_model_blocks) return Error{ErrorCode::Limit, "graph exceeds 1024 blocks"};

  namespace ir = wetstone::ir;
  std::set<BlockId> ids;
  for (const auto& block : blocks) {
    if (block.id == ir::invalid_block || !ids.insert(block.id).second)
      return Error{ErrorCode::InvalidInput, "duplicate or reserved block ID"};
    if (block.successors.size() > 2)
      return Error{ErrorCode::Unsupported, "at most two successors per block are supported"};
    if (block.successors.size() == 2 && block.successors[0] == block.successors[1])
      return Error{ErrorCode::InvalidInput, "block successors must be distinct"};
  }
  if (!ids.contains(entry)) return Error{ErrorCode::InvalidInput, "entry block is absent"};

  ir::Function graph;
  graph.entry = entry;
  graph.parameters = {0};
  graph.blocks.reserve(blocks.size());
  for (const auto& block : blocks) {
    for (const auto target : block.successors)
      if (!ids.contains(target)) return Error{ErrorCode::InvalidInput, "edge targets an absent block"};
    ir::Instruction terminator;
    if (block.successors.empty()) {
      terminator.opcode = ir::Opcode::Return;
    } else if (block.successors.size() == 1) {
      terminator.opcode = ir::Opcode::Jump;
    } else {
      // A structural edge list has no conditions. A dummy parameter satisfies
      // Wetstone's branch shape without claiming either edge is infeasible.
      terminator.opcode = ir::Opcode::Branch;
      terminator.arguments = {ir::Operand::ref(0)};
    }
    terminator.targets = block.successors;
    graph.blocks.push_back({block.id, {std::move(terminator)}});
  }

  const auto reach = wetstone::graph::reachable(graph);
  GraphSummary result;
  result.reachable.assign(reach.begin(), reach.end());
  for (auto id : ids)
    if (!reach.contains(id)) result.unreachable.push_back(id);
  for (const auto& component : wetstone::graph::strongly_connected_components(graph)) {
    const auto id = *component.begin();
    if (!reach.contains(id)) continue;
    if (component.size() == 1) {
      const auto successors = graph.successors(id);
      if (std::find(successors.begin(), successors.end(), id) == successors.end()) continue;
    }
    result.reachable_cycles.emplace_back(component.begin(), component.end());
  }
  std::sort(result.reachable_cycles.begin(), result.reachable_cycles.end());
  return result;
}

} // namespace rtmbin
