#pragma once
#include "GraphColoringRegalloc.hpp"

namespace firestone::regalloc {
struct CoalescingResult {
    Problem problem;
    std::vector<ValueId> representative; // Original value -> contracted value.
};
// Briggs's conservative criterion on a uniform register domain. For irregular
// domains conservative mode declines contraction; preferences still guide coloring.
inline CoalescingResult coalesce(const Problem& p, const Machine& m, bool aggressive = false) {
    validate(p, m);
    std::vector<std::set<ValueId>> groups(p.size());
    auto owner = detail::ids(p);
    for (ValueId v = 0; v < p.size(); ++v) groups[v].insert(v);
    auto domain = [&](ValueId group) {
        std::set<RegisterId> out; bool first = true;
        for (auto v : groups[group]) {
            auto regs = candidates(p.value(v), m); std::set<RegisterId> current(regs.begin(), regs.end());
            if (first) { out = std::move(current); first = false; }
            else for (auto it = out.begin(); it != out.end();) {
                if (!current.count(*it)) it = out.erase(it); else ++it;
            }
        }
        return out;
    };
    auto neighbors = [&](ValueId group) {
        std::set<ValueId> out;
        for (auto v : groups[group]) for (auto n : p.neighbors(v)) if (owner[n] != group) out.insert(owner[n]);
        return out;
    };
    auto affinities = p.affinities();
    std::stable_sort(affinities.begin(), affinities.end(), [](auto a, auto b) { return a.weight > b.weight; });
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto affinity : affinities) {
            auto a = owner[affinity.a], b = owner[affinity.b];
            if (a == b || neighbors(a).count(b)) continue;
            auto da = domain(a), db = domain(b); std::set<RegisterId> common;
            std::set_intersection(da.begin(), da.end(), db.begin(), db.end(), std::inserter(common, common.end()));
            if (common.empty()) continue;
            auto adjacent = neighbors(a); auto nb = neighbors(b); adjacent.insert(nb.begin(), nb.end());
            std::optional<RegisterId> fixed;
            for (auto group : {a, b}) for (auto v : groups[group]) if (p.value(v).fixed) fixed = p.value(v).fixed;
            bool fixed_conflict = false;
            if (fixed) for (auto n : adjacent) for (auto v : groups[n])
                if (p.value(v).fixed && m.aliases(*fixed, *p.value(v).fixed)) fixed_conflict = true;
            if (fixed_conflict) continue;
            if (!aggressive) {
                bool uniform = da == db;
                for (auto r : common) for (auto s : common) if (r != s && m.aliases(r, s)) uniform = false;
                std::size_t high_degree = 0;
                for (auto n : adjacent) {
                    if (domain(n) != common) uniform = false;
                    if (neighbors(n).size() >= common.size()) ++high_degree;
                }
                if (!uniform || high_degree >= common.size()) continue;
            }
            for (auto v : groups[b]) { groups[a].insert(v); owner[v] = a; }
            groups[b].clear(); changed = true;
        }
    }
    CoalescingResult out; out.representative.resize(p.size());
    for (ValueId group = 0; group < groups.size(); ++group) if (!groups[group].empty()) {
        Value merged = p.value(*groups[group].begin());
        merged.ranges.clear(); merged.uses.clear(); merged.allowed = domain(group);
        merged.rematerialization_cost.reset(); merged.spill_cost = 0; merged.frequency = 0;
        for (auto v : groups[group]) {
            const auto& x = p.value(v);
            merged.spill_cost = detail::add_cost(merged.spill_cost, x.spill_cost);
            merged.frequency = detail::add_cost(merged.frequency, x.frequency);
            merged.spillable = merged.spillable && x.spillable;
            if (x.fixed) merged.fixed = x.fixed;
            merged.spill_size = std::max(merged.spill_size, x.spill_size);
            merged.spill_alignment = std::max(merged.spill_alignment, x.spill_alignment);
        }
        auto id = out.problem.add(std::move(merged));
        for (auto v : groups[group]) out.representative[v] = id;
    }
    for (ValueId v = 0; v < p.size(); ++v) for (auto n : p.neighbors(v))
        out.problem.interfere(out.representative[v], out.representative[n]);
    for (auto a : p.affinities()) out.problem.prefer(out.representative[a.a], out.representative[a.b], a.weight);
    return out;
}
struct CoalescingRegalloc {
    Allocation allocate(const Problem& p, const Machine& m, bool aggressive = false) const {
        auto contracted = coalesce(p, m, aggressive);
        auto colored = GraphColoringRegalloc{}.allocate(contracted.problem, m);
        Allocation out; out.locations.resize(p.size());
        for (ValueId v = 0; v < p.size(); ++v) out.locations[v] = colored.locations[contracted.representative[v]];
        detail::finish(p, m, out); return out;
    }
};
} // namespace firestone::regalloc
