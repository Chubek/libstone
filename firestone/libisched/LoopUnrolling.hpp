#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct Unrolled { Graph graph; std::vector<std::pair<NodeId, std::size_t>> origin; };
struct LoopUnrolling {
    Unrolled unroll(const Graph& g, std::size_t factor) const {
        if (factor == 0 || (g.size() && factor > std::numeric_limits<std::size_t>::max() / g.size()))
            throw std::invalid_argument("invalid unroll factor");
        g.topological_order(); Unrolled out;
        for (std::size_t iteration = 0; iteration < factor; ++iteration) for (NodeId id = 0; id < g.size(); ++id) {
            out.graph.add(g.node(id)); out.origin.emplace_back(id, iteration);
        }
        for (std::size_t iteration = 0; iteration < factor; ++iteration) for (const auto& d : g.dependencies()) {
            auto target_iteration = checked_add(iteration, d.distance);
            out.graph.depend(iteration * g.size() + d.from, (target_iteration % factor) * g.size() + d.to,
                             d.delay, d.kind, target_iteration / factor);
        }
        return out;
    }
};
} // namespace firestone::isched
