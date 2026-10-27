#pragma once
#include "ListSched.hpp"
namespace firestone::isched {
struct Bundle { Cycle cycle = 0; std::vector<NodeId> operations; };
struct VLIWResult { Schedule schedule; std::vector<Bundle> bundles; };
struct VLIWPacking {
    VLIWResult pack(const Graph& g, const Machine& m) const {
        VLIWResult out; out.schedule = ListSched{}.schedule(g, m);
        std::map<Cycle, std::vector<NodeId>> bundles;
        for (auto id : out.schedule.order()) bundles[out.schedule.start[id]].push_back(id);
        for (auto& b : bundles) out.bundles.push_back({b.first, std::move(b.second)});
        return out;
    }
};
} // namespace firestone::isched
