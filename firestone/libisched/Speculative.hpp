#pragma once
#include "ProfileDrivenSched.hpp"
namespace firestone::isched {
struct SpeculativeResult { Graph graph; Schedule schedule; std::vector<Dependency> relaxed; };
struct Speculative {
    SpeculativeResult schedule(const Graph& g, const Machine& m = {}) const {
        validate(g, m); SpeculativeResult out;
        for (NodeId id = 0; id < g.size(); ++id) out.graph.add(g.node(id));
        for (const auto& d : g.dependencies()) {
            if (d.kind == DependencyKind::Control && g.node(d.to).speculatable) out.relaxed.push_back(d);
            else out.graph.depend(d.from, d.to, d.delay, d.kind, d.distance);
        }
        out.schedule = ProfileDrivenSched{}.schedule(out.graph, m); return out;
    }
};
} // namespace firestone::isched
