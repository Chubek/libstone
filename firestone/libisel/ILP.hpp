#pragma once
#include "Common.hpp"
namespace firestone::isel {
struct CoverILP {
    std::vector<double> objective;
    std::map<NodeId, std::vector<std::size_t>> exactly_one;
};
struct ILP {
    CoverILP formulate(const Problem& p) const {
        CoverILP model;
        for (auto n : p.live) model.exactly_one[n] = {};
        for (std::size_t i = 0; i < p.tiles.size(); ++i) {
            model.objective.push_back(p.rules[p.tiles[i].rule].cost);
            for (auto n : p.tiles[i].covered) model.exactly_one[n].push_back(i);
        }
        return model;
    }
    // Dependency-free binary branch-and-bound, with an inspectable ILP model.
    Selection select(const Graph& g, const Rules& r, const std::vector<NodeId>& roots,
                     std::size_t budget = 100000) const { return detail::exact(Problem(g, r, roots), budget); }
};
} // namespace firestone::isel
