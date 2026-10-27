#pragma once
#include "PBQPRegalloc.hpp"
namespace firestone::regalloc {
struct AllocationILP {
    struct Variable { ValueId value; Location location; double cost; };
    struct AffinityProduct { std::size_t left, right; double saving; }; // y = x_left AND x_right.
    std::vector<Variable> binary_variables;
    std::vector<std::vector<std::size_t>> exactly_one;
    std::vector<std::pair<std::size_t, std::size_t>> at_most_one;
    std::vector<AffinityProduct> products;
    double constant = 0; // Objective = constant + unary costs - product savings.
};
struct ILPRegalloc {
    AllocationILP formulate(const Problem& p, const Machine& m) const {
        auto pbqp = PBQPRegalloc{}.formulate(p, m); AllocationILP out;
        for (ValueId v = 0; v < p.size(); ++v) {
            std::vector<std::size_t> row;
            for (std::size_t c = 0; c < pbqp.domains[v].size(); ++c) {
                row.push_back(out.binary_variables.size());
                out.binary_variables.push_back({v, pbqp.domains[v][c], pbqp.costs[v][c]});
            }
            out.exactly_one.push_back(std::move(row));
        }
        for (const auto& e : pbqp.edges) for (std::size_t a = 0; a < e.costs.size(); ++a)
            for (std::size_t b = 0; b < e.costs[a].size(); ++b) if (!std::isfinite(e.costs[a][b]))
                out.at_most_one.emplace_back(out.exactly_one[e.a][a], out.exactly_one[e.b][b]);
        for (auto affinity : p.affinities()) {
            out.constant = detail::add_cost(out.constant, affinity.weight);
            for (auto a : out.exactly_one[affinity.a]) for (auto b : out.exactly_one[affinity.b]) {
                auto x = out.binary_variables[a].location, y = out.binary_variables[b].location;
                if (x.kind == LocationKind::Register && x == y) out.products.push_back({a, b, affinity.weight});
            }
        }
        return out;
    }
    Allocation allocate(const Problem& p, const Machine& m, std::size_t budget = 100000) const {
        return PBQPRegalloc{}.allocate(p, m, budget);
    }
};
} // namespace firestone::regalloc
