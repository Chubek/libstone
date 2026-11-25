#include "LoopDependenceAnalysis.hpp"
#include <iterator>

namespace wetstone::alias {

DependenceResult LoopDependenceAnalysis::analyze(const DirectAnalysis& direct,
                                                const path::Loop& loop) {
    return analyze(direct, loop, direct.accesses());
}

DependenceResult LoopDependenceAnalysis::analyze(const DirectAnalysis& direct,
    const path::Loop& loop, std::span<const MemoryAccess> accesses) {
    if (direct.reachability(loop.header) != Reachability::Reachable ||
        !loop.body.contains(loop.header)) return {false, {}};

    // Merge all reachable latches of the header. The legacy natural_loops()
    // can return one candidate per backedge and include unreachable incoming
    // predecessors in that candidate; neither should limit this query.
    std::set<ir::BlockId> body{loop.header};
    std::vector<ir::BlockId> work;
    const auto incoming = direct.predecessors_.find(loop.header);
    if (incoming == direct.predecessors_.end()) return {false, {}};
    bool has_latch = false;
    for (const auto pred : incoming->second) {
        const auto dom = direct.dominators_.find(pred);
        if (dom != direct.dominators_.end() && dom->second.contains(loop.header)) {
            has_latch = true;
            if (body.insert(pred).second) work.push_back(pred);
        }
    }
    if (!has_latch) return {false, {}};
    while (!work.empty()) {
        const auto block = work.back();
        work.pop_back();
        const auto preds = direct.predecessors_.find(block);
        if (preds == direct.predecessors_.end()) continue;
        for (const auto pred : preds->second) {
            const auto dom = direct.dominators_.find(pred);
            if (dom == direct.dominators_.end()) continue;
            if (!dom->second.contains(loop.header)) return {false, {}};
            if (body.insert(pred).second) work.push_back(pred);
        }
    }
    for (const auto block : loop.body) {
        const auto reach = direct.reachability(block);
        if (reach == Reachability::Unknown ||
            (reach == Reachability::Reachable && !body.contains(block))) return {false, {}};
    }

    DependenceResult result;
    std::vector<const MemoryAccess*> selected;
    for (const auto& access : accesses) {
        const auto reach = direct.reachability(access.block, access.instruction);
        if (reach == Reachability::Unknown) {
            // An unknown block might have been meant to describe a loop
            // member; bad indices in known outside blocks are irrelevant.
            if (body.contains(access.block) ||
                direct.reachability(access.block) == Reachability::Unknown)
                result.complete = false;
            continue;
        }
        if (body.contains(access.block)) selected.push_back(&access);
    }
    for (const auto* source : selected)
        for (const auto* sink : selected) {
            auto pair = direct.dependences_in(*source, *sink, &body, loop.header);
            result.complete &= pair.complete;
            result.dependences.insert(result.dependences.end(),
                std::make_move_iterator(pair.dependences.begin()),
                std::make_move_iterator(pair.dependences.end()));
        }
    return result;
}

DependenceResult analyze_loop_dependences(const DirectAnalysis& direct, const path::Loop& loop) {
    return LoopDependenceAnalysis::analyze(direct, loop);
}

DependenceResult analyze_loop_dependences(const DirectAnalysis& direct, const path::Loop& loop,
                                          std::span<const MemoryAccess> accesses) {
    return LoopDependenceAnalysis::analyze(direct, loop, accesses);
}

} // namespace wetstone::alias
