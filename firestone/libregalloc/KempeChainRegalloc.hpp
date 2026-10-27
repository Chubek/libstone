#pragma once
#include "GraphColoringRegalloc.hpp"
namespace firestone::regalloc {
struct KempeChainRegalloc {
    Allocation recolor(const Problem& p, const Machine& m, Allocation out, std::size_t budget = 10000) const {
        if (!verify(p, m, out).empty()) throw std::invalid_argument("Kempe recoloring requires a valid allocation");
        for (ValueId v = 0; v < p.size(); ++v) if (out.locations[v].kind != LocationKind::Register) {
            bool placed = false;
            for (auto a : detail::preferences(p, m, out.locations, v)) {
                if (detail::fits(p, m, out.locations, v, a)) { out.locations[v] = Location::reg(a); placed = true; break; }
                for (RegisterId b = 0; b < m.registers.size() && budget; ++b) {
                    if (m.aliases(a, b)) continue;
                    auto trial = out.locations; std::set<ValueId> chain; std::vector<ValueId> pending;
                    for (auto n : p.neighbors(v)) if (trial[n] == Location::reg(a)) pending.push_back(n);
                    bool possible = !pending.empty();
                    while (!pending.empty() && possible) {
                        auto n = pending.back(); pending.pop_back();
                        if (!chain.insert(n).second) continue;
                        if (budget == 0 || p.value(n).fixed) { possible = false; break; }
                        --budget;
                        auto other = trial[n].index == a ? b : a;
                        if (!legal(p.value(n), m, other)) { possible = false; break; }
                        for (auto next : p.neighbors(n)) if (!chain.count(next) &&
                            (trial[next] == Location::reg(a) || trial[next] == Location::reg(b))) pending.push_back(next);
                    }
                    if (!possible) continue;
                    for (auto n : chain) trial[n] = Location::reg(trial[n].index == a ? b : a);
                    trial[v] = Location::reg(a);
                    // Alias units and constrained domains can invalidate a textbook swap.
                    for (ValueId n = 0; n < p.size(); ++n) if (trial[n].kind == LocationKind::Register &&
                        !detail::fits(p, m, trial, n, trial[n].index)) { possible = false; break; }
                    if (possible) { out.locations = std::move(trial); placed = true; break; }
                }
                if (placed) break;
            }
        }
        out.optimal = false; detail::finish(p, m, out); return out;
    }
    Allocation allocate(const Problem& p, const Machine& m, std::size_t budget = 10000) const {
        auto out = GraphColoringRegalloc{}.allocate(p, m);
        return out.feasible ? recolor(p, m, std::move(out), budget) : out;
    }
};
} // namespace firestone::regalloc
