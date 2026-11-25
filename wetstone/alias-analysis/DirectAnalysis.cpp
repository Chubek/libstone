#include "DirectAnalysis.hpp"
#include "../utils/SSAValidation.hpp"
#include <array>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace wetstone::alias {
namespace {

// These instructions do not access abstract program memory. No default
// "pure" case: an unrecognized opcode is an opaque access.
bool known_nonmemory(ir::Opcode opcode) {
    using enum ir::Opcode;
    switch (opcode) {
    case Nop: case Constant: case Copy: case Add: case Sub: case Mul: case Div:
    case Mod: case Neg: case BitAnd: case BitOr: case BitXor: case Shl: case Shr:
    case Eq: case Ne: case Lt: case Le: case Gt: case Ge: case Phi: case AddressOf:
    case Return: case Jump: case Branch: case Assert: case Assume: case Input:
    case Sanitize:
        return true;
    default:
        return false;
    }
}

} // namespace

DirectAnalysis::DirectAnalysis(const ir::Function& function, const CallSummaries& summaries,
                               std::size_t points_to_iteration_limit)
    : function_(function) {
    try {
        function_.validate();
        for (const auto& block : function_.blocks)
            for (const auto& inst : block.instructions)
                if (inst.opcode != ir::Opcode::Jump && inst.opcode != ir::Opcode::Branch &&
                    !inst.targets.empty())
                    throw std::invalid_argument("unmodeled CFG edges");
        valid_cfg_ = true;
        dominators_ = graph::dominators(function_);
        ir::verify_ssa(function_);
        valid_ssa_ = true;
    } catch (const std::invalid_argument&) {
        // Structurally invalid CFGs cannot prove unreachability; a valid CFG
        // with bad SSA can still provide ordering, but not pointer facts.
    }

    if (valid_ssa_) points_to_ = analyze_points_to(function_, points_to_iteration_limit);
    else points_to_.converged = false;

    for (const auto parameter : function_.parameters)
        definitions_.emplace(parameter, Position{ir::invalid_block, 0});
    for (std::size_t b = 0; b < function_.blocks.size(); ++b) {
        const auto& block = function_.blocks[b];
        if (valid_cfg_) {
            block_indices_.emplace(block.id, b);
            auto& successors = successors_[block.id];
            successors = function_.successors(block.id);
            for (const auto successor : successors)
                predecessors_[successor].push_back(block.id);
        }
    }
    for (const auto& block : function_.blocks) {
        const auto reach = reachability(block.id);
        for (std::size_t n = 0; n < block.instructions.size(); ++n) {
            const auto& inst = block.instructions[n];
            if (inst.result) definitions_.emplace(*inst.result, Position{block.id, n});
            if (reach == Reachability::Unreachable) continue;
            if (auto call = call_site(inst, block.id, n)) {
                const auto fx = call_effects(*call, summaries);
                calls_.push_back(std::move(*call));
                // Lifetime effects and opaque calls conservatively clobber
                // unknown storage; no per-argument location is implied.
                if (fx.unknown_call) accesses_.push_back({block.id, n, AccessKind::Unknown, {}});
                else if (fx.writes || fx.allocates || fx.frees)
                    accesses_.push_back({block.id, n,
                                         fx.reads ? AccessKind::ReadWrite : AccessKind::Write, {}});
                else if (fx.reads) accesses_.push_back({block.id, n, AccessKind::Read, {}});
            } else if (auto access = memory_access(inst, block.id, n)) {
                accesses_.push_back(std::move(*access));
            } else if (!known_nonmemory(inst.opcode)) {
                accesses_.push_back({block.id, n, AccessKind::Unknown, {}});
            }
        }
    }
}

Reachability DirectAnalysis::reachability(ir::BlockId block) const {
    if (!block_indices_.contains(block)) return Reachability::Unknown;
    return dominators_.contains(block) ? Reachability::Reachable : Reachability::Unreachable;
}

Reachability DirectAnalysis::reachability(ir::BlockId block, std::size_t instruction) const {
    const auto found = block_indices_.find(block);
    if (found == block_indices_.end() ||
        instruction >= function_.blocks[found->second].instructions.size())
        return Reachability::Unknown;
    return reachability(block);
}

AliasResult DirectAnalysis::alias(const MemoryLocation& lhs, const MemoryLocation& rhs) const {
    auto a = lhs, b = rhs;
    if (!valid_ssa_ || !definitions_.contains(a.pointer)) a.pointer = ir::invalid_value;
    if (!valid_ssa_ || !definitions_.contains(b.pointer)) b.pointer = ir::invalid_value;
    return wetstone::alias::alias(a, b, points_to_);
}

bool DirectAnalysis::available(ir::ValueId pointer, const MemoryAccess& access) const {
    if (!valid_ssa_) return false;
    const auto def = definitions_.find(pointer);
    if (def == definitions_.end()) return false;
    const auto position = def->second;
    if (position.block == ir::invalid_block) return true;
    if (position.block == access.block) return position.instruction < access.instruction;
    return dominators_.at(access.block).contains(position.block);
}

DependenceResult DirectAnalysis::dependences(const MemoryAccess& source,
                                             const MemoryAccess& sink) const {
    return dependences_in(source, sink, nullptr, ir::invalid_block);
}

DependenceResult DirectAnalysis::dependences_in(const MemoryAccess& source, const MemoryAccess& sink,
    const std::set<ir::BlockId>* body, ir::BlockId header) const {
    DependenceResult result;
    const auto source_reach = reachability(source.block, source.instruction);
    const auto sink_reach = reachability(sink.block, sink.instruction);
    if (source_reach == Reachability::Unknown || sink_reach == Reachability::Unknown) {
        result.complete = false;
        detail::append_dependences(result, source, sink, IterationRelation::Unknown,
                                   AliasResult::MayAlias);
        return result;
    }
    if (source_reach == Reachability::Unreachable || sink_reach == Reachability::Unreachable)
        return result;
    if (source.kind == AccessKind::Read && sink.kind == AccessKind::Read)
        return result;

    auto a = source.location, b = sink.location;
    if (!available(a.pointer, source)) a.pointer = ir::invalid_value;
    if (!available(b.pointer, sink)) b.pointer = ir::invalid_value;
    const auto alias_result = alias(a, b);
    const bool shared_pointer = a.pointer != ir::invalid_value && a.pointer == b.pointer;
    // With an identical pointer id, byte-range disjointness is conditional on
    // observing the same dynamic definition, except for empty locations.
    if (alias_result == AliasResult::NoAlias &&
        (!shared_pointer || a.size == 0 || b.size == 0)) return result;
    const auto def = shared_pointer ? definitions_.at(a.pointer) : Position{ir::invalid_block, 0};
    const bool track_definition = def.block != ir::invalid_block;

    // Mark paths that can execute the sink, separately for zero vs nonzero
    // header crossings. Track redefinitions on the exact between-access
    // segment (suffix of source, whole intermediate blocks, prefix of sink).
    struct Paths { bool possible = false, redefined = false; };
    std::array<Paths, 2> paths{};
    if (source.block == sink.block && source.instruction < sink.instruction)
        paths[0].possible = true;
    using State = std::tuple<ir::BlockId, bool, bool>;
    std::set<State> seen;
    std::vector<State> work;
    const auto follow = [&](ir::BlockId block, bool crossed, bool redefined) {
        for (const auto next : successors_.at(block)) {
            if (body && !body->contains(next)) continue;
            const State state{next, crossed || (body && next == header), redefined};
            if (seen.insert(state).second) work.push_back(state);
        }
    };
    follow(source.block, false, track_definition && def.block == source.block &&
                                def.instruction > source.instruction);
    while (!work.empty()) {
        const auto [block, crossed, redefined] = work.back();
        work.pop_back();
        const bool executes_def = track_definition && def.block == block;
        if (block == sink.block) {
            auto& relation = paths[crossed];
            relation.possible = true;
            relation.redefined |= redefined || (executes_def && def.instruction < sink.instruction);
        }
        follow(block, crossed, redefined || executes_def);
    }

    if (paths[0].possible)
        detail::append_dependences(result, source, sink,
            body ? IterationRelation::SameIteration : IterationRelation::Unknown,
            paths[0].redefined ? AliasResult::MayAlias : alias_result);
    if (paths[1].possible)
        detail::append_dependences(result, source, sink, IterationRelation::LoopCarried,
            paths[1].redefined ? AliasResult::MayAlias : alias_result);
    return result;
}

} // namespace wetstone::alias
