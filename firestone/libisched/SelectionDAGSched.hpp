#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct SelectionDAGSched {
    Schedule schedule(const Graph& g, const Machine& m = {}) const {
        auto h = detail::heights(g); std::vector<double> priorities(g.size());
        std::vector<std::size_t> uses(g.size());
        for (const auto& d : g.dependencies()) if (d.kind == DependencyKind::Data) ++uses[d.from];
        for (NodeId id = 0; id < g.size(); ++id) {
            double released = 0;
            for (const auto& d : g.dependencies()) if (d.to == id && uses[d.from] == 1) ++released;
            priorities[id] = static_cast<double>(h[id]) + released / (static_cast<double>(g.size()) + 1);
        }
        return detail::list(g, m, priorities);
    }
};
} // namespace firestone::isched
