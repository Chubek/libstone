#pragma once
#include "Common.hpp"

namespace firestone::regalloc {
using Liveness = ir::Liveness;
using ir::compute_liveness;
struct SSAProblem {
    Problem problem;
    Liveness liveness;
    std::map<std::pair<ir::BlockId, std::size_t>, Position> positions; // Instruction read slots; writes are slot + 1.
    std::map<ir::BlockId, Position> entries, exits;
};
inline SSAProblem analyze_ssa(const ir::SimpleSSAGraph& ssa,
                             const std::function<void(ValueId, Value&)>& configure = {}) {
    SSAProblem out; out.liveness = ir::compute_liveness(ssa);
    std::vector<std::set<Position>> points(ssa.value_count());
    std::vector<Value> values(ssa.value_count());
    Position next = 0;
    for (auto b : ssa.cfg().blocks()) {
        const auto& ins = ssa.cfg().block(b).instructions;
        if (next > std::numeric_limits<Position>::max() - 2 ||
            ins.size() > (std::numeric_limits<Position>::max() - next - 2) / 2)
            throw std::overflow_error("liveness position overflow");
        out.entries[b] = next++;
        for (std::size_t i = 0; i < ins.size(); ++i) { out.positions[{b, i}] = next; next += 2; }
        out.exits[b] = next++;
    }
    for (auto b : ssa.cfg().blocks()) {
        const auto& ins = ssa.cfg().block(b).instructions;
        auto live = out.liveness.live_out.at(b);
        for (auto v : live) points[v].insert(out.exits.at(b));
        for (std::size_t i = ins.size(); i-- > 0;) {
            const auto& inst = ins[i]; auto position = out.positions.at({b, i});
            for (auto v : live) points[v].insert(position + 1);
            if (inst.result) { points[*inst.result].insert(position + 1); live.erase(*inst.result); }
            for (std::size_t k = 0; k < inst.operands.size(); ++k) if (inst.operands[k].kind == ir::OperandKind::Value) {
                auto v = inst.operands[k].value;
                if (inst.opcode == ir::Opcode::Phi) {
                    auto edge_position = out.exits.at(inst.incoming_blocks[k]);
                    values[v].uses.push_back(edge_position); points[v].insert(edge_position);
                } else { live.insert(v); values[v].uses.push_back(position); }
            }
            for (auto v : live) points[v].insert(position);
        }
        for (auto v : live) points[v].insert(out.entries.at(b));
    }
    if (!ssa.cfg().blocks().empty()) for (const auto& d : ssa.definitions()) if (d.second.parameter)
        points[d.first].insert(out.entries.at(ssa.cfg().entry()));
    for (ValueId v = 0; v < values.size(); ++v) {
        auto& x = values[v];
        for (auto pos : points[v]) {
            if (!x.ranges.empty() && x.ranges.back().end == pos) x.ranges.back().end = pos + 1;
            else x.ranges.push_back({pos, pos + 1});
        }
        std::sort(x.uses.begin(), x.uses.end()); x.spill_cost = static_cast<double>(x.uses.size() + 1);
        if (configure) configure(v, x);
        out.problem.add(std::move(x));
    }
    auto interference = ir::InterferenceGraph::from_ssa(ssa);
    for (ValueId v = 0; v < values.size(); ++v) for (auto n : interference.neighbors(v)) if (n > v) out.problem.interfere(v, n);
    for (auto b : ssa.cfg().blocks()) for (const auto& inst : ssa.cfg().block(b).instructions)
        if (inst.opcode == ir::Opcode::Phi && inst.result) for (const auto& operand : inst.operands)
            if (operand.kind == ir::OperandKind::Value) out.problem.prefer(*inst.result, operand.value);
    return out;
}
inline Problem from_ssa(const ir::SimpleSSAGraph& ssa, const std::function<void(ValueId, Value&)>& configure = {}) {
    return analyze_ssa(ssa, configure).problem;
}
inline Problem from_interference(const ir::InterferenceGraph& graph, std::size_t value_count) {
    Problem out;
    for (ValueId v = 0; v < value_count; ++v) out.add();
    for (ValueId v = 0; v < value_count; ++v) for (auto n : graph.neighbors(v)) out.interfere(v, n);
    return out;
}
} // namespace firestone::regalloc
