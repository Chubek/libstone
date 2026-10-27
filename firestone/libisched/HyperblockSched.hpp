#pragma once
#include "TraceSched.hpp"
namespace firestone::isched {
struct HyperblockSched {
    // The caller supplies if-converted operations with target predicate names.
    // Predicates are retained in the result; their def-use edges must be explicit.
    RegionResult schedule(const Graph& g, const std::vector<ir::BlockId>& blocks, const Machine& m = {}) const {
        std::set<ir::BlockId> members(blocks.begin(), blocks.end());
        if (members.size() != blocks.size()) throw std::invalid_argument("duplicate hyperblock block");
        Graph predicated;
        for (NodeId id = 0; id < g.size(); ++id) {
            const auto& op = g.node(id);
            if (!members.count(op.block)) throw std::invalid_argument("operation outside hyperblock");
            if (!blocks.empty() && op.block != blocks.front() && op.predicate.empty() && !op.speculatable)
                throw std::invalid_argument("conditional operation needs a predicate or speculation safety");
            predicated.add(op);
        }
        for (const auto& d : g.dependencies()) {
            if (d.kind == DependencyKind::Control && (!g.node(d.to).predicate.empty() || g.node(d.to).speculatable)) continue;
            predicated.depend(d.from, d.to, d.delay, d.kind, d.distance);
        }
        RegionResult out{blocks, std::move(predicated), {}};
        out.schedule = ProfileDrivenSched{}.schedule(out.graph, m); return out;
    }
};
} // namespace firestone::isched
