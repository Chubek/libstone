#pragma once
#include "Common.hpp"

namespace firestone::regalloc {
enum class Priority { SpillCost, UseDensity, Frequency, LoopDepth, Degree, SourceOrder };
struct GreedyOptions {
    Priority priority = Priority::SpillCost;
    bool eviction = true;
    std::size_t recolor_depth = 2;
    std::size_t recolor_budget = 1000;
};
namespace detail {
inline double priority(const Problem& p, ValueId v, Priority policy) {
    const auto& x = p.value(v);
    switch (policy) {
    case Priority::UseDensity: {
        double length = 0;
        for (auto r : x.ranges) length += static_cast<double>(r.end - r.begin);
        return static_cast<double>(x.uses.size()) / std::max(1.0, length);
    }
    case Priority::Frequency: return x.frequency;
    case Priority::LoopDepth: return static_cast<double>(x.loop_depth);
    case Priority::Degree: return static_cast<double>(p.neighbors(v).size());
    case Priority::SourceOrder: return 0;
    case Priority::SpillCost: return spill_cost(x);
    }
    return 0;
}
// Transactional bounded recoloring. Every blocker must move; precolors never move.
inline bool recolor(const Problem& p, const Machine& m, std::vector<Location>& locations,
                    ValueId v, RegisterId r, std::size_t depth, std::size_t& budget, std::set<ValueId> locked = {}) {
    if (budget == 0 || !legal(p.value(v), m, r) || locked.count(v)) return false;
    --budget;
    auto saved = locations;
    locked.insert(v); locations[v] = Location::reg(r);
    for (auto n : p.neighbors(v)) if (locations[n].kind == LocationKind::Register && m.aliases(r, locations[n].index)) {
        if (depth == 0 || p.value(n).fixed || locked.count(n)) { locations = std::move(saved); return false; }
        bool moved = false;
        for (auto alt : preferences(p, m, locations, n)) if (alt != locations[n].index &&
            recolor(p, m, locations, n, alt, depth - 1, budget, locked)) { moved = true; break; }
        if (!moved) { locations = std::move(saved); return false; }
    }
    return true;
}
inline Allocation greedy(const Problem& p, const Machine& m, const GreedyOptions& options, std::vector<ValueId> order) {
    validate(p, m); auto out = initial(p);
    auto budget = options.recolor_budget;
    for (auto v : order) {
        if (p.value(v).fixed) continue;
        bool placed = false;
        auto regs = preferences(p, m, out.locations, v);
        for (auto r : regs) if (fits(p, m, out.locations, v, r)) {
            out.locations[v] = Location::reg(r); placed = true; break;
        }
        if (!placed && options.recolor_depth) for (auto r : regs)
            if (recolor(p, m, out.locations, v, r, options.recolor_depth, budget)) { placed = true; break; }
        if (!placed && options.eviction) {
            double best = std::numeric_limits<double>::infinity(); RegisterId chosen = invalid_id;
            std::vector<ValueId> victims;
            for (auto r : regs) {
                double cost = 0; bool possible = true; std::vector<ValueId> blockers;
                for (auto n : p.neighbors(v)) if (out.locations[n].kind == LocationKind::Register && m.aliases(r, out.locations[n].index)) {
                    if (p.value(n).fixed || !p.value(n).spillable) { possible = false; break; }
                    cost = add_cost(cost, spill_cost(p.value(n))); blockers.push_back(n);
                }
                if (possible && cost < best) { best = cost; chosen = r; victims = std::move(blockers); }
            }
            if (chosen != invalid_id && (!p.value(v).spillable || best < spill_cost(p.value(v)))) {
                for (auto n : victims) out.locations[n] = spill(p.value(n));
                out.locations[v] = Location::reg(chosen); placed = true;
            }
        }
        if (!placed) out.locations[v] = spill(p.value(v));
    }
    // A second chance for evicted values; assignments are now stable.
    for (auto v : order) if (out.locations[v].kind != LocationKind::Register) {
        for (auto r : preferences(p, m, out.locations, v)) if (fits(p, m, out.locations, v, r)) {
            out.locations[v] = Location::reg(r); break;
        }
    }
    finish(p, m, out); return out;
}
} // namespace detail
struct GreedyRegalloc {
    Allocation allocate(const Problem& p, const Machine& m, GreedyOptions options = {}) const {
        auto order = detail::ids(p);
        std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
            if (p.value(a).spillable != p.value(b).spillable) return !p.value(a).spillable;
            if (options.priority == Priority::SourceOrder) return a < b;
            auto x = detail::priority(p, a, options.priority), y = detail::priority(p, b, options.priority);
            if (x != y) return x > y;
            return p.neighbors(a).size() > p.neighbors(b).size();
        });
        return detail::greedy(p, m, options, std::move(order));
    }
};
} // namespace firestone::regalloc
