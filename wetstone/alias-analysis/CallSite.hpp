#pragma once

#include "../interprocedural/EffectSummary.hpp"
#include "../utils/IR.hpp"
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wetstone::alias {

struct CallSite {
    ir::BlockId block = ir::invalid_block;
    std::size_t instruction = 0;
    std::string callee;
    std::vector<ir::Operand> operands;
    std::optional<ir::ValueId> result;
    bool valid = true;

    [[nodiscard]] bool is_direct() const noexcept { return valid && !callee.empty(); }
};

[[nodiscard]] std::optional<CallSite>
call_site(const ir::Instruction& instruction, ir::BlockId block, std::size_t index);

// Structural enumeration, including unreachable calls. A CFG consumer must
// filter by its own reachability facts.
[[nodiscard]] std::vector<CallSite> call_sites(const ir::Function& function);

using CallSummaries = std::map<std::string, interprocedural::EffectSummary>;

// Supplied summaries must overapproximate the callee's transitive effects.
// Missing, indirect, malformed, or unknown summaries produce all effects.
[[nodiscard]] interprocedural::EffectSummary
call_effects(const CallSite& site, const CallSummaries& summaries = {});

} // namespace wetstone::alias
