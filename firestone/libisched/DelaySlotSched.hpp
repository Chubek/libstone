#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct DelaySlots { std::vector<NodeId> order; std::vector<std::optional<NodeId>> slots; };
struct DelaySlotSched {
    // Fill architectural delay slots with independent, nontrapping operations
    // from before the branch. This transforms order, not cycle timing.
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
        for (std::size_t pos = branch_position; pos > 0 && selected.size() < count;) {
            auto id = order[--pos]; if (!g.node(id).speculatable) continue;
            bool safe = true;
            for (const auto& d : g.dependencies()) if (d.from == id && position.at(d.to) <= branch_position) safe = false;
            if (safe) selected.push_back(id);
        }
        std::reverse(selected.begin(), selected.end());
        for (auto id : selected) out.order.erase(std::find(out.order.begin(), out.order.end(), id));
        auto where = std::find(out.order.begin(), out.order.end(), branch) + 1;
        out.order.insert(where, selected.begin(), selected.end());
        for (std::size_t i = 0; i < selected.size(); ++i) out.slots[i] = selected[i];
        return out;
    }
};
} // namespace firestone::isched
