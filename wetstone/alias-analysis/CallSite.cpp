#include "CallSite.hpp"

#include <algorithm>
#include <utility>

namespace wetstone::alias {

std::optional<CallSite> call_site(const ir::Instruction& instruction, ir::BlockId block,
                                  std::size_t index) {
    if (instruction.opcode != ir::Opcode::Call) return std::nullopt;
    const bool valid = instruction.targets.empty() && instruction.phi_predecessors.empty() &&
        instruction.result != ir::invalid_value &&
        std::none_of(instruction.arguments.begin(), instruction.arguments.end(), [](const auto& operand) {
            return !operand.is_constant && operand.value == ir::invalid_value;
        });
    return CallSite{block, index, instruction.symbol, instruction.arguments, instruction.result, valid};
}

std::vector<CallSite> call_sites(const ir::Function& function) {
    std::vector<CallSite> sites;
    for (const auto& block : function.blocks)
        for (std::size_t i = 0; i < block.instructions.size(); ++i)
            if (auto site = call_site(block.instructions[i], block.id, i))
                sites.push_back(std::move(*site));
    return sites;
}

interprocedural::EffectSummary call_effects(const CallSite& site, const CallSummaries& summaries) {
    if (site.is_direct()) {
        const auto summary = summaries.find(site.callee);
        if (summary != summaries.end() && !summary->second.unknown_call) return summary->second;
    }
    return {true, true, true, true, true};
}

} // namespace wetstone::alias
