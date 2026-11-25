#include "MustMay.hpp"

#include "../pointer-analysis/PointerAnalyzer.hpp"
#include "../utils/SSAValidation.hpp"
#include <stdexcept>
#include <string>

namespace wetstone::alias {

AliasResult range_alias(const ByteRange& a, const ByteRange& b) {
    if (!a.valid() || !b.valid()) return AliasResult::MayAlias;
    if (a.begin == a.end || b.begin == b.end || a.end <= b.begin || b.end <= a.begin)
        return AliasResult::NoAlias;
    return AliasResult::MustAlias;
}

AliasResult alias(const MemoryLocation& a, const MemoryLocation& b,
                  const pointer::PointsToResult& points_to) {
    if (a.size == 0 || b.size == 0) return AliasResult::NoAlias;
    if (a.pointer == ir::invalid_value || b.pointer == ir::invalid_value)
        return AliasResult::MayAlias;

    const auto left_range = byte_range(a);
    const auto right_range = byte_range(b);
    if (a.pointer == b.pointer) {
        if (!left_range || !right_range) return AliasResult::MayAlias;
        return range_alias(*left_range, *right_range);
    }

    // Require known offsets and reject unrepresentable endpoints. Different
    // object identities separate valid in-object accesses, including an access
    // with unknown size. A shared ID, even a singleton allocation site, gives
    // no guarantee of overlapping offsets or of one dynamic allocation.
    if (!a.offset || !b.offset || (a.size && !left_range) || (b.size && !right_range) ||
        !points_to.converged) return AliasResult::MayAlias;
    const auto x = points_to.variables.find(a.pointer);
    const auto y = points_to.variables.find(b.pointer);
    if (x == points_to.variables.end() || y == points_to.variables.end() ||
        x->second.empty() || y->second.empty() ||
        x->second.contains(pointer::unknown_object) || y->second.contains(pointer::unknown_object))
        return AliasResult::MayAlias;
    for (const auto object : x->second)
        if (y->second.contains(object)) return AliasResult::MayAlias;
    return AliasResult::NoAlias;
}

pointer::PointsToResult analyze_points_to(const ir::Function& function, std::size_t iteration_limit) {
    try {
        ir::verify_ssa(function);
        // Wetstone's CFG helpers only follow edges of Jump and Branch.
        // Reject unmodeled edges before using reachability to prune Phi inputs.
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                if (instruction.opcode != ir::Opcode::Jump &&
                    instruction.opcode != ir::Opcode::Branch && !instruction.targets.empty())
                    throw std::invalid_argument("unmodeled CFG edges");
    } catch (const std::invalid_argument&) {
        pointer::PointsToResult result;
        result.converged = false;
        return result;
    }

    const auto reachable = graph::reachable(function);
    pointer::PointerAnalyzer analyzer;
    const auto unknown = [&](ir::ValueId result) {
        analyzer.constraints.push_back({pointer::InclusionConstraint::Kind::Address, result, 0,
                                        pointer::unknown_object});
    };
    const auto propagate = [&](ir::ValueId result, const ir::Operand& input) {
        if (input.is_constant)
            unknown(result);
        else
            analyzer.copy(result, input.value);
    };

    for (const auto parameter : function.parameters) unknown(parameter);
    for (const auto& block : function.blocks) {
        if (!reachable.contains(block.id)) continue;
        for (std::size_t n = 0; n < block.instructions.size(); ++n) {
            const auto& instruction = block.instructions[n];
            if (!instruction.result) continue;
            const auto result = *instruction.result;
            if (!instruction.targets.empty() ||
                (instruction.opcode != ir::Opcode::Phi && !instruction.phi_predecessors.empty())) {
                unknown(result);
                continue;
            }
            switch (instruction.opcode) {
            case ir::Opcode::AddressOf:
                if (instruction.arguments.empty() && !instruction.symbol.empty())
                    analyzer.address(result, "global:" + instruction.symbol);
                else
                    unknown(result);
                break;
            case ir::Opcode::Alloc:
                // Fresh allocations at one static site share an abstract ID.
                // This ID alone does not identify a particular live instance.
                if (instruction.arguments.empty())
                    analyzer.address(result, "alloc:" + std::to_string(block.id) + ":" + std::to_string(n));
                else
                    unknown(result);
                break;
            case ir::Opcode::Copy:
                if (instruction.arguments.size() == 1)
                    propagate(result, instruction.arguments.front());
                else
                    unknown(result);
                break;
            case ir::Opcode::Phi: {
                bool has_input = false;
                for (std::size_t k = 0; k < instruction.arguments.size(); ++k) {
                    if (!reachable.contains(instruction.phi_predecessors[k])) continue;
                    has_input = true;
                    propagate(result, instruction.arguments[k]);
                }
                // Entry can be entered without taking any CFG predecessor edge.
                if (!has_input || block.id == function.entry) unknown(result);
                break;
            }
            default:
                unknown(result);
                break;
            }
        }
    }
    return analyzer.solve(iteration_limit);
}

} // namespace wetstone::alias
