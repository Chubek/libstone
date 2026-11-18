#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct DelaySlots { std::vector<NodeId> order; std::vector<std::optional<NodeId>> slots; };
struct DelaySlotSched {
    // Fill architectural delay slots with nontrapping operations from before
    // the branch, preserving dependencies even between moved operations.
    // This transforms order, not cycle timing.
    DelaySlots fill(const Graph& g, const std::vector<NodeId>& order, NodeId branch, std::size_t count) const {
        g.node(branch);
        if (order.size() != g.size()) throw std::invalid_argument("incomplete input order");
        std::map<NodeId, std::size_t> position;
        for (std::size_t i = 0; i < order.size(); ++i) { g.node(order[i]); if (!position.emplace(order[i], i).second) throw std::invalid_argument("duplicate operation"); }
        for (const auto& d : g.dependencies()) if (d.distance || position.at(d.from) >= position.at(d.to))
            throw std::invalid_argument("delay-slot input must be a topological order");
        DelaySlots out{order, std::vector<std::optional<NodeId>>(count)};
        std::vector<NodeId> selected;
        const auto branch_position = position.at(branch);
        std::vector<std::vector<NodeId>> blocking(g.size());
        for (const auto& d : g.dependencies()) if (position.at(d.to) <= branch_position) blocking[d.from].push_back(d.to);
        std::vector<bool> moved(g.size());
        for (std::size_t pos = branch_position; pos > 0 && selected.size() < count;) {
            auto id = order[--pos]; if (!g.node(id).speculatable) continue;
            if (std::all_of(blocking[id].begin(), blocking[id].end(), [&](auto user) { return moved[user]; })) {
                selected.push_back(id);
                moved[id] = true;
            }
        }
        std::reverse(selected.begin(), selected.end());
        out.order.erase(std::remove_if(out.order.begin(), out.order.end(), [&](auto id) { return moved[id]; }), out.order.end());
        auto where = std::find(out.order.begin(), out.order.end(), branch) + 1;
        out.order.insert(where, selected.begin(), selected.end());
        for (std::size_t i = 0; i < selected.size(); ++i) out.slots[i] = selected[i];
        return out;
    }
};
} // namespace firestone::isched
