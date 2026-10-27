#pragma once
#include "Common.hpp"

namespace firestone::regalloc {
struct BipartiteMatchingRegalloc {
    // Minimum-cost matching for a simultaneous live set (an interference clique).
    // Private spill vertices allow independent spills. Registers must not alias.
    Allocation allocate(const Problem& p, const Machine& m) const {
        validate(p, m);
        for (ValueId a = 0; a < p.size(); ++a) for (ValueId b = a + 1; b < p.size(); ++b)
            if (!p.interferes(a, b)) throw std::invalid_argument("matching requires a simultaneous live set");
        for (RegisterId a = 0; a < m.registers.size(); ++a) for (RegisterId b = a + 1; b < m.registers.size(); ++b)
            if (m.aliases(a, b)) throw std::invalid_argument("matching requires disjoint registers");
        struct Edge { std::size_t to, reverse; bool capacity; double cost; };
        const auto base = 1 + p.size(), sink = base + m.registers.size() + p.size();
        std::vector<std::vector<Edge>> graph(sink + 1);
        auto add = [&](std::size_t a, std::size_t b, double cost) {
            auto x = graph[a].size(), y = graph[b].size();
            graph[a].push_back({b, y, true, cost}); graph[b].push_back({a, x, false, -cost});
        };
        for (ValueId v = 0; v < p.size(); ++v) {
            add(0, 1 + v, 0);
            for (auto r : candidates(p.value(v), m)) add(1 + v, base + r, m.registers[r].cost);
            if (detail::spill(p.value(v)).kind != LocationKind::Unassigned) add(1 + v, base + m.registers.size() + v, detail::spill_cost(p.value(v)));
        }
        for (auto node = base; node < sink; ++node) add(node, sink, 0);
        Allocation out = detail::initial(p);
        for (std::size_t flow = 0; flow < p.size(); ++flow) {
            std::vector<double> distance(graph.size(), std::numeric_limits<double>::infinity());
            std::vector<std::pair<std::size_t, std::size_t>> previous(graph.size(), {invalid_id, invalid_id}); distance[0] = 0;
            for (std::size_t pass = 1; pass < graph.size(); ++pass) {
                bool changed = false;
                for (std::size_t a = 0; a < graph.size(); ++a) if (std::isfinite(distance[a]))
                    for (std::size_t i = 0; i < graph[a].size(); ++i) {
                        const auto& edge = graph[a][i]; auto candidate = distance[a] + edge.cost;
                        if (edge.capacity && candidate < distance[edge.to]) {
                            if (!std::isfinite(candidate)) throw std::overflow_error("matching cost overflow");
                            distance[edge.to] = candidate; previous[edge.to] = {a, i}; changed = true;
                        }
                    }
                if (!changed) break;
            }
            if (previous[sink].first == invalid_id) { out.stop = Stop::Infeasible; detail::finish(p, m, out); return out; }
            for (auto node = sink; node != 0;) {
                auto prev = previous[node]; auto& edge = graph[prev.first][prev.second];
                edge.capacity = false; graph[node][edge.reverse].capacity = true; node = prev.first;
            }
        }
        for (ValueId v = 0; v < p.size(); ++v) for (const auto& edge : graph[1 + v])
            if (edge.to >= base && !edge.capacity) out.locations[v] = edge.to < base + m.registers.size() ?
                Location::reg(edge.to - base) : detail::spill(p.value(v));
        detail::finish(p, m, out); out.optimal = true; return out;
    }
};
} // namespace firestone::regalloc
