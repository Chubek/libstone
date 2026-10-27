#pragma once
#include "GreedyRegalloc.hpp"
#include "Liveness.hpp"

namespace firestone::regalloc {
struct ChordalOrder {
    std::vector<ValueId> elimination;
    bool chordal = true;
};
// Maximum cardinality search, with an explicit perfect-elimination check.
inline ChordalOrder chordal_order(const Problem& p) {
    ChordalOrder out; std::vector<std::size_t> weight(p.size()); std::set<ValueId> remaining;
    for (ValueId v = 0; v < p.size(); ++v) remaining.insert(v);
    while (!remaining.empty()) {
        auto v = *remaining.begin();
        for (auto n : remaining) if (weight[n] > weight[v]) v = n;
        remaining.erase(v); out.elimination.push_back(v);
        for (auto n : p.neighbors(v)) if (remaining.count(n)) ++weight[n];
    }
    std::reverse(out.elimination.begin(), out.elimination.end());
    std::vector<std::size_t> position(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) position[out.elimination[i]] = i;
    for (auto v : out.elimination) {
        std::vector<ValueId> later;
        for (auto n : p.neighbors(v)) if (position[n] > position[v]) later.push_back(n);
        for (auto a : later) for (auto b : later) if (a != b && !p.interferes(a, b)) out.chordal = false;
    }
    return out;
}
struct SSAChordalRegalloc {
    Allocation allocate(const Problem& p, const Machine& m) const {
        auto order = chordal_order(p);
        if (!order.chordal) throw std::invalid_argument("SSA chordal coloring requires a chordal interference graph");
        std::reverse(order.elimination.begin(), order.elimination.end());
        return detail::greedy(p, m, {Priority::SourceOrder, false, 0, 0}, std::move(order.elimination));
    }
    Allocation allocate(const ir::SimpleSSAGraph& ssa, const Machine& m) const { return allocate(from_ssa(ssa), m); }
};
} // namespace firestone::regalloc
