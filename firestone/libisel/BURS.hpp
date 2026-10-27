#pragma once
#include "Common.hpp"
namespace firestone::isel {
struct BURSLabel {
    double cost = std::numeric_limits<double>::infinity();
    std::optional<std::size_t> tile;
    std::size_t state = 0;
};
struct BURS {
    // Runtime bottom-up rewrite states. State signatures record every matching
    // production and its relative cost; equal signatures share a state number.
    // The grammar has one value nonterminal; target types are rule guards.
    std::map<NodeId, BURSLabel> label(const Problem& p) const {
        std::map<NodeId, BURSLabel> labels;
        std::map<std::vector<std::pair<std::size_t, double>>, std::size_t> states;
        for (auto id : p.live) {
            BURSLabel label; std::vector<std::pair<std::size_t, double>> signature;
            auto candidates = p.at_root.find(id);
            if (candidates != p.at_root.end()) for (auto ti : candidates->second) {
                const auto& tile = p.tiles[ti]; double cost = p.rules[tile.rule].cost;
                std::set<NodeId> inputs;
                for (const auto& b : tile.bindings) inputs.insert(b.second);
                for (auto child : inputs) cost += labels.at(child).cost;
                if (!std::isfinite(cost)) continue;
                signature.emplace_back(tile.rule, cost);
                if (cost < label.cost) { label.cost = cost; label.tile = ti; }
            }
            for (auto& item : signature) item.second -= label.cost;
            auto state = states.emplace(signature, states.size()); label.state = state.first->second;
            labels.emplace(id, label);
        }
        return labels;
    }
    Selection select(const Graph& graph, const Rules& rules, const std::vector<NodeId>& roots) const {
        Problem p(graph, rules, roots); const auto labels = label(p);
        std::set<NodeId> seen; std::vector<std::size_t> chosen;
        std::function<bool(NodeId)> reduce = [&](NodeId id) {
            if (!seen.insert(id).second) return true;
            const auto& entry = labels.at(id); if (!entry.tile) return false;
            chosen.push_back(*entry.tile);
            for (const auto& b : p.tiles[*entry.tile].bindings) if (!reduce(b.second)) return false;
            return true;
        };
        for (auto id : roots) if (!reduce(id)) return detail::failed(p);
        auto out = materialize(p, chosen);
        std::map<NodeId, std::size_t> uses;
        for (auto id : roots) ++uses[id];
        for (auto id : p.live) for (auto child : graph.node(id).inputs) ++uses[child];
        out.optimal = std::all_of(uses.begin(), uses.end(), [](const auto& item) { return item.second <= 1; });
        return out;
    }
};
} // namespace firestone::isel
