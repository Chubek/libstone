#pragma once

#include "CallSite.hpp"
#include "DependenceTypes.hpp"
#include "../utils/Graph.hpp"
#include <map>
#include <set>

namespace wetstone::alias {

class LoopDependenceAnalysis;

// Owns a snapshot of a Wetstone IR function. Only the supplied direct-call
// summaries are trusted; lack of a summary is an opaque memory effect.
// IR producers must preserve effects: an unknown source operation lowered to
// Nop or Input cannot be recovered by this analysis.
class DirectAnalysis {
public:
    explicit DirectAnalysis(const ir::Function&, const CallSummaries& = {},
                            std::size_t points_to_iteration_limit = 10000);

    [[nodiscard]] const ir::Function& function() const noexcept { return function_; }
    [[nodiscard]] const pointer::PointsToResult& points_to() const noexcept { return points_to_; }
    // Includes reachable program accesses and conservatively unknown effects.
    // On an invalid CFG, retains syntactically observed accesses from all blocks.
    [[nodiscard]] const std::vector<MemoryAccess>& accesses() const noexcept { return accesses_; }
    [[nodiscard]] const std::vector<CallSite>& calls() const noexcept { return calls_; }
    [[nodiscard]] bool valid_cfg() const noexcept { return valid_cfg_; }

    // Structural CFG reachability, not branch feasibility.
    [[nodiscard]] Reachability reachability(ir::BlockId) const;
    [[nodiscard]] Reachability reachability(ir::BlockId, std::size_t instruction) const;

    // The locations are compared at one observation of their SSA values. Use
    // dependences() instead for different dynamic instruction executions.
    [[nodiscard]] AliasResult alias(const MemoryLocation&, const MemoryLocation&) const;
    [[nodiscard]] DependenceResult dependences(const MemoryAccess& source,
                                               const MemoryAccess& sink) const;

private:
    friend class LoopDependenceAnalysis;
    struct Position {
        ir::BlockId block;
        std::size_t instruction;
    };

    [[nodiscard]] bool available(ir::ValueId, const MemoryAccess&) const;
    [[nodiscard]] DependenceResult dependences_in(
        const MemoryAccess&, const MemoryAccess&, const std::set<ir::BlockId>* body,
        ir::BlockId header) const;

    ir::Function function_;
    bool valid_cfg_ = false;
    bool valid_ssa_ = false;
    graph::Dominators dominators_;
    std::map<ir::BlockId, std::size_t> block_indices_;
    std::map<ir::BlockId, std::vector<ir::BlockId>> successors_;
    std::map<ir::BlockId, std::vector<ir::BlockId>> predecessors_;
    std::map<ir::ValueId, Position> definitions_;
    pointer::PointsToResult points_to_;
    std::vector<MemoryAccess> accesses_;
    std::vector<CallSite> calls_;
};

} // namespace wetstone::alias
