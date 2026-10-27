#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct ProfileDrivenSched {
    Schedule schedule(const Graph& g, const Machine& m = {}) const {
        auto h = detail::heights(g); std::vector<double> priorities(g.size());
        for (NodeId id = 0; id < g.size(); ++id) priorities[id] = g.node(id).frequency * static_cast<double>(h[id]);
        return detail::list(g, m, priorities);
    }
};
} // namespace firestone::isched
