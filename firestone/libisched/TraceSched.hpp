#pragma once
#include "Speculative.hpp"
#include "../libir/ControlFlowGraph.hpp"
namespace firestone::isched {
using EdgeFrequencies = std::map<std::pair<ir::BlockId, ir::BlockId>, double>;
struct RegionResult {
    std::vector<ir::BlockId> blocks;
    Graph graph;
    Schedule schedule;
};
namespace detail {
inline std::vector<ir::BlockId> hot_path(const ir::ControlFlowGraph& cfg, ir::BlockId entry,
                                       const EdgeFrequencies& weights, bool single_entry) {
    cfg.block(entry);
    for (const auto& item : weights) {
        if (!std::isfinite(item.second) || item.second < 0) throw std::invalid_argument("invalid edge frequency");
        const auto successors = cfg.successors(item.first.first);
        if (std::find(successors.begin(), successors.end(), item.first.second) == successors.end())
            throw std::invalid_argument("profile references a nonedge");
    }
    std::vector<ir::BlockId> path{entry}; std::set<ir::BlockId> seen{entry};
    for (;;) {
        auto successors = cfg.successors(path.back()); ir::BlockId next = ir::invalid_id; double weight = -1;
        for (auto b : successors) if (!seen.count(b) && (!single_entry || cfg.predecessors(b).size() == 1)) {
            auto it = weights.find({path.back(), b}); double w = it == weights.end() ? 0 : it->second;
            if (w > weight) { weight = w; next = b; }
        }
        if (next == ir::invalid_id) break;
        seen.insert(next); path.push_back(next);
    }
    return path;
}
inline Graph region_graph(const Graph& input, const std::vector<ir::BlockId>& blocks, bool allow_motion) {
    Graph out; std::map<ir::BlockId, std::size_t> positions;
    for (std::size_t i = 0; i < blocks.size(); ++i)
        if (!positions.emplace(blocks[i], i).second) throw std::invalid_argument("duplicate region block");
    for (NodeId id = 0; id < input.size(); ++id) {
        if (!positions.count(input.node(id).block)) throw std::invalid_argument("operation outside region");
        out.add(input.node(id));
    }
    for (const auto& d : input.dependencies()) out.depend(d.from, d.to, d.delay, d.kind, d.distance);
    // No implicit compensation code: cross-block motion requires explicit safety.
    for (NodeId a = 0; a < input.size(); ++a) for (NodeId b = 0; b < input.size(); ++b)
        if (positions.at(input.node(a).block) < positions.at(input.node(b).block) &&
            (!allow_motion || !input.node(b).speculatable))
            out.depend(a, b, input.node(a).latency, DependencyKind::Control);
    return out;
}
} // namespace detail
struct TraceSched {
    std::vector<ir::BlockId> trace(const ir::ControlFlowGraph& cfg, ir::BlockId entry,
                                 const EdgeFrequencies& weights = {}) const { return detail::hot_path(cfg, entry, weights, false); }
    RegionResult schedule(const Graph& g, const std::vector<ir::BlockId>& blocks, const Machine& m = {}) const {
        // Multi-entry traces retain block boundaries; no unrequested CFG cloning.
        RegionResult out{blocks, detail::region_graph(g, blocks, false), {}};
        out.schedule = ProfileDrivenSched{}.schedule(out.graph, m); return out;
    }
};
} // namespace firestone::isched
