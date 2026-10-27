#pragma once
#include "Common.hpp"
namespace firestone::regalloc {
struct ScavengeResult {
    std::optional<RegisterId> reg;
    std::vector<ValueId> save_restore; // Preserve these values around the temporary's use.
};
struct RegisterScavenging {
    ScavengeResult scavenge(const Problem& p, const Machine& m, const Allocation& allocation,
                           const std::set<ValueId>& live, const Value& temporary = {}, bool allow_save = true) const {
        if (!verify(p, m, allocation).empty()) throw std::invalid_argument("scavenging requires a valid allocation");
        Problem temporary_problem; temporary_problem.add(temporary); validate(temporary_problem, m);
        for (auto v : live) p.value(v);
        ScavengeResult out; double cheapest = std::numeric_limits<double>::infinity();
        for (auto r : candidates(temporary, m)) {
            std::vector<ValueId> victims; double cost = 0; bool possible = true;
            for (auto v : live) if (allocation.locations[v].kind == LocationKind::Register && m.aliases(r, allocation.locations[v].index)) {
                if (!allow_save || p.value(v).fixed || !p.value(v).spillable) { possible = false; break; }
                victims.push_back(v); cost = detail::add_cost(cost, p.value(v).spill_cost);
            }
            if (possible && (cost < cheapest || (cost == cheapest && victims.size() < out.save_restore.size()))) {
                out.reg = r; out.save_restore = std::move(victims); cheapest = cost;
            }
        }
        return out;
    }
};
} // namespace firestone::regalloc
