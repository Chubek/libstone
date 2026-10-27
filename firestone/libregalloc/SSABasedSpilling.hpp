#pragma once
#include "Liveness.hpp"
#include "GreedyRegalloc.hpp"
namespace firestone::regalloc {
enum class SpillActionKind { StoreDefinition, ReloadUse, RematerializeUse, StoreParameter };
struct SpillAction {
    SpillActionKind kind;
    ValueId value;
    ir::BlockId block;
    std::size_t instruction;
    std::optional<ir::BlockId> successor; // A phi use is on this outgoing edge, not in its destination block.
    Location home;
};
struct SSASpillPlan {
    SSAProblem analysis;
    Allocation allocation;
    std::vector<SpillAction> actions;
};
struct SSABasedSpilling {
    Allocation allocate(const ir::SimpleSSAGraph& ssa, const Machine& m,
                        const std::function<void(ValueId, Value&)>& configure = {}) const {
        return GreedyRegalloc{}.allocate(from_ssa(ssa, configure), m);
    }
    SSASpillPlan plan(const ir::SimpleSSAGraph& ssa, const Machine& m,
                      const std::function<void(ValueId, Value&)>& configure = {}) const {
        SSASpillPlan out; out.analysis = analyze_ssa(ssa, configure);
        out.allocation = GreedyRegalloc{}.allocate(out.analysis.problem, m);
        if (!out.allocation.feasible) return out;
        for (const auto& def : ssa.definitions()) {
            auto home = out.allocation.locations[def.first];
            if (home.kind != LocationKind::Stack) continue;
            out.actions.push_back({def.second.parameter ? SpillActionKind::StoreParameter : SpillActionKind::StoreDefinition,
                def.first, def.second.parameter ? ssa.cfg().entry() : def.second.block,
                def.second.parameter ? 0 : def.second.instruction, {}, home});
        }
        for (auto block : ssa.cfg().blocks()) {
            const auto& instructions = ssa.cfg().block(block).instructions;
            for (std::size_t i = 0; i < instructions.size(); ++i) {
                const auto& inst = instructions[i]; std::set<std::pair<ValueId, ir::BlockId>> emitted;
                for (std::size_t k = 0; k < inst.operands.size(); ++k) if (inst.operands[k].kind == ir::OperandKind::Value) {
                    auto v = inst.operands[k].value; auto home = out.allocation.locations[v];
                    if (home.kind != LocationKind::Stack && home.kind != LocationKind::Rematerialize) continue;
                    auto source = inst.opcode == ir::Opcode::Phi ? inst.incoming_blocks[k] : block;
                    if (!emitted.insert({v, source}).second) continue;
                    auto action = home.kind == LocationKind::Stack ? SpillActionKind::ReloadUse : SpillActionKind::RematerializeUse;
                    out.actions.push_back({action, v, source, inst.opcode == ir::Opcode::Phi ?
                        ssa.cfg().block(source).instructions.size() - 1 : i,
                        inst.opcode == ir::Opcode::Phi ? std::optional<ir::BlockId>(block) : std::nullopt, home});
                }
            }
        }
        return out;
    }
};
} // namespace firestone::regalloc
