#pragma once
#include "GreedyRegalloc.hpp"

namespace firestone::regalloc {
struct ColoringOptions {
    bool optimistic = true;
    std::size_t recolor_depth = 0;
    std::size_t recolor_budget = 1000;
};
struct GraphColoringRegalloc {
    Allocation allocate(const Problem& p, const Machine& m, ColoringOptions options = {}) const {
        validate(p, m); auto out = detail::initial(p);
        std::set<ValueId> remaining; std::vector<ValueId> stack;
        for (ValueId v = 0; v < p.size(); ++v) if (!p.value(v).fixed) remaining.insert(v);
        auto degree = [&](ValueId v) {
            std::size_t count = 0;
            for (auto n : p.neighbors(v)) if (remaining.count(n) || p.value(n).fixed) ++count;
            return count;
        };
        while (!remaining.empty()) {
            auto chosen = invalid_id;
            for (auto v : remaining) if (degree(v) < candidates(p.value(v), m).size()) { chosen = v; break; }
            bool potential_spill = chosen == invalid_id;
            if (potential_spill) {
                double cheapest = std::numeric_limits<double>::infinity();
                for (auto v : remaining) if (p.value(v).spillable) {
                    auto score = detail::spill_cost(p.value(v)) / static_cast<double>(std::max<std::size_t>(1, degree(v)));
                    if (chosen == invalid_id || score < cheapest) { cheapest = score; chosen = v; }
                }
                if (chosen == invalid_id) chosen = *remaining.begin();
            }
            remaining.erase(chosen);
            if (potential_spill && !options.optimistic && p.value(chosen).spillable) out.locations[chosen] = detail::spill(p.value(chosen));
            else stack.push_back(chosen);
        }
        auto budget = options.recolor_budget;
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            auto v = *it; bool placed = false;
            for (auto r : detail::preferences(p, m, out.locations, v)) if (detail::fits(p, m, out.locations, v, r)) {
                out.locations[v] = Location::reg(r); placed = true; break;
            }
            if (!placed && options.recolor_depth) for (auto r : detail::preferences(p, m, out.locations, v))
                if (detail::recolor(p, m, out.locations, v, r, options.recolor_depth, budget)) { placed = true; break; }
            if (!placed) out.locations[v] = detail::spill(p.value(v));
        }
        detail::finish(p, m, out); return out;
    }
};
} // namespace firestone::regalloc
