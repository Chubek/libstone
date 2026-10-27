#pragma once
#include "Common.hpp"
namespace firestone::isel {
// Reconstruct legal tiles from the original graph/rules, not claimed coverage.
inline ir::Diagnostics verify(const Graph& graph, const Rules& rules,
                              const std::vector<NodeId>& roots, const Selection& result) {
    Problem p(graph, rules, roots); ir::Diagnostics errors;
    if (result.roots != roots) ir::diagnose(errors, "roots", "selection outputs differ from the requested roots");
    if (result.stop == Stop::Infeasible) ir::diagnose(errors, "status", "selection has no feasible cover");
    if (result.optimal && result.stop != Stop::Complete) ir::diagnose(errors, "status", "limited selection cannot claim optimality");
    std::vector<std::size_t> chosen; std::set<NodeId> defined;
    for (const auto& inst : result.instructions) {
        if (!defined.insert(inst.result).second) ir::diagnose(errors, "duplicate", "duplicate result", inst.result);
        for (auto input : inst.inputs) if (input == inst.result || !defined.count(input))
            ir::diagnose(errors, "input", "input is not defined before use", inst.result);
        std::optional<std::size_t> matched;
        auto candidates = p.at_root.find(inst.result);
        if (candidates != p.at_root.end()) for (auto ti : candidates->second)
            if (p.tiles[ti].rule == inst.rule) { matched = ti; break; }
        if (!matched) { ir::diagnose(errors, "rule", "instruction has no legal matching rule", inst.result); continue; }
        chosen.push_back(*matched);
        const auto& tile = p.tiles[*matched]; const auto& rule = rules[tile.rule];
        auto names = rule.operand_order;
        if (names.empty()) for (const auto& b : tile.bindings) names.push_back(b.first);
        std::vector<NodeId> inputs; for (const auto& name : names) inputs.push_back(tile.bindings.at(name));
        if (inst.opcode != rule.instruction || inst.latency != rule.latency || inst.resource != rule.resource ||
            inst.occupancy != rule.occupancy || inst.units != rule.units || inst.inputs != inputs || inst.input_names != names ||
            inst.covered != std::vector<NodeId>(tile.covered.begin(), tile.covered.end()))
            ir::diagnose(errors, "instruction", "instruction differs from its matched target rule", inst.result);
    }
    try {
        auto expected = materialize(p, chosen);
        if (!std::isfinite(result.cost) || result.cost != expected.cost)
            ir::diagnose(errors, "cost", "incorrect selection cost");
    } catch (const std::invalid_argument& error) {
        ir::diagnose(errors, "cover", error.what());
    }
    return errors;
}
} // namespace firestone::isel
