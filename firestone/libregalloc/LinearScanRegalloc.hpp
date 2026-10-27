#pragma once
#include "Common.hpp"

namespace firestone::regalloc {
struct LinearScanOptions {
    bool lifetime_holes = false;
    bool second_chance = false;
};
namespace detail {
inline Problem interval_problem(const Problem& p, bool holes) {
    Problem out;
    for (ValueId v = 0; v < p.size(); ++v) {
        auto x = p.value(v);
        if (x.ranges.empty()) throw std::invalid_argument("linear scan requires live intervals");
        if (!holes) x.ranges = {{x.ranges.front().begin, x.ranges.back().end}};
        out.add(std::move(x));
    }
    for (ValueId v = 0; v < p.size(); ++v) for (auto n : p.explicit_neighbors(v)) if (n > v) out.interfere(v, n);
    for (auto a : p.affinities()) out.prefer(a.a, a.b, a.weight);
    return out;
}
} // namespace detail
struct LinearScanRegalloc {
    Allocation allocate(const Problem& input, const Machine& m, LinearScanOptions options = {}) const {
        validate(input, m);
        auto p = detail::interval_problem(input, options.lifetime_holes);
        // Hulls may create precolor conflicts absent from the original hole-aware problem.
        validate(p, m); auto out = detail::initial(p); auto order = detail::ids(p);
        std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
            return p.value(a).ranges.front().begin < p.value(b).ranges.front().begin;
        });
        for (auto v : order) {
            if (p.value(v).fixed) continue;
            bool placed = false;
            auto regs = detail::preferences(p, m, out.locations, v);
            for (auto r : regs) if (detail::fits(p, m, out.locations, v, r)) {
                out.locations[v] = Location::reg(r); placed = true; break;
            }
            if (!placed) {
                RegisterId chosen = invalid_id; Position furthest = 0; std::vector<ValueId> victims;
                for (auto r : regs) {
                    bool possible = true; Position end = std::numeric_limits<Position>::max(); std::vector<ValueId> blockers;
                    for (auto n : p.neighbors(v)) if (out.locations[n].kind == LocationKind::Register && m.aliases(r, out.locations[n].index)) {
                        if (p.value(n).fixed || !p.value(n).spillable) { possible = false; break; }
                        end = std::min(end, p.value(n).ranges.back().end); blockers.push_back(n);
                    }
                    // In the basic uniform case there is exactly one active victim.
                    if (possible && end > furthest && (!p.value(v).spillable || end > p.value(v).ranges.back().end)) {
                        chosen = r; furthest = end; victims = std::move(blockers);
                    }
                }
                if (chosen != invalid_id) {
                    for (auto n : victims) out.locations[n] = detail::spill(p.value(n));
                    out.locations[v] = Location::reg(chosen); placed = true;
                }
            }
            if (!placed) out.locations[v] = detail::spill(p.value(v));
        }
        if (options.second_chance) for (auto v : order) if (out.locations[v].kind != LocationKind::Register) {
            for (auto r : detail::preferences(p, m, out.locations, v)) if (detail::fits(p, m, out.locations, v, r)) {
                out.locations[v] = Location::reg(r); break;
            }
        }
        detail::finish(input, m, out); return out;
    }
};
} // namespace firestone::regalloc
