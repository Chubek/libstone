#pragma once
#include "ILP.hpp"
namespace firestone::isel {
struct SMTModel {
    CoverILP cover;
    // Additional caller-supplied Boolean clauses over tile variables.
    // Literal k+1 denotes tile k; negative literals denote negation.
    std::vector<std::vector<std::int64_t>> clauses;
};
struct UniversalSMT {
    SMTModel formulate(const Problem& p, const std::vector<std::vector<std::int64_t>>& clauses = {}) const {
        for (const auto& clause : clauses) for (auto lit : clause)
            if (lit == 0 || lit == std::numeric_limits<std::int64_t>::min() ||
                static_cast<std::uint64_t>(lit < 0 ? -lit : lit) > p.tiles.size())
                throw std::invalid_argument("invalid SMT tile literal");
        return {ILP{}.formulate(p), clauses};
    }
    Selection select(const Problem& p, const std::vector<std::vector<std::int64_t>>& clauses = {},
                     std::size_t budget = 100000) const {
        const auto model = formulate(p, clauses);
        std::vector<int> assignment(p.tiles.size(), -1);
        Selection best = detail::failed(p); double cost = std::numeric_limits<double>::infinity();
        const auto& rows = model.cover.exactly_one;
        std::size_t explored = 0; bool limit = false;
        std::function<void(std::size_t, double)> solve = [&](std::size_t next, double subtotal) {
            if (explored >= budget) { limit = true; return; } ++explored;
            if (subtotal >= cost) return;
            for (const auto& row : rows) {
                std::size_t yes = 0, unknown = 0;
                for (auto i : row.second) { yes += assignment[i] == 1; unknown += assignment[i] < 0; }
                if (yes > 1 || (yes == 0 && unknown == 0)) return;
            }
            for (const auto& clause : clauses) {
                bool possible = false;
                for (auto lit : clause) {
                    auto a = assignment[static_cast<std::size_t>(lit < 0 ? -lit : lit) - 1];
                    if (a < 0 || (a == 1) == (lit > 0)) possible = true;
                }
                if (!possible) return;
            }
            if (next == assignment.size()) {
                std::vector<std::size_t> chosen;
                for (std::size_t i = 0; i < assignment.size(); ++i) if (assignment[i] == 1) chosen.push_back(i);
                best = materialize(p, chosen); cost = subtotal; return;
            }
            assignment[next] = 1; solve(next + 1, subtotal + p.rules[p.tiles[next].rule].cost);
            if (!limit) { assignment[next] = 0; solve(next + 1, subtotal); }
            assignment[next] = -1;
        };
        solve(0, 0); best.explored = explored; best.optimal = !limit && best.stop == Stop::Complete;
        if (limit) best.stop = Stop::SearchLimit;
        return best;
    }
    Selection select(const Graph& g, const Rules& r, const std::vector<NodeId>& roots,
                     std::size_t budget = 100000) const { return select(Problem(g, r, roots), {}, budget); }
};
} // namespace firestone::isel
