#pragma once
#include "Common.hpp"

namespace firestone::regalloc {
// Regions are value-priority groups, not independent allocation problems.
// A value appears in at most one group; unlisted values form a final group.
// Global interference is retained, so no boundary copies are required.
struct AllocationRegion {
    std::vector<ValueId> values;
    double frequency = 1;
};
namespace detail {
inline std::vector<ValueId> region_order(const Problem& p,
                                         const std::vector<AllocationRegion>& regions,
                                         bool intervals) {
    std::vector<bool> seen(p.size());
    std::vector<std::size_t> priority(regions.size());
    std::iota(priority.begin(), priority.end(), 0);
    for (const auto& region : regions) {
        if (!std::isfinite(region.frequency) || region.frequency < 0)
            throw std::invalid_argument("invalid region frequency");
        for (auto v : region.values) {
            if (v >= p.size() || seen[v]) throw std::invalid_argument("unknown or repeated regional value");
            seen[v] = true;
        }
    }
    std::stable_sort(priority.begin(), priority.end(), [&](auto a, auto b) {
        return regions[a].frequency > regions[b].frequency;
    });
    auto less = [&](ValueId a, ValueId b) {
        if (intervals) {
            auto x = p.value(a).ranges.front().begin, y = p.value(b).ranges.front().begin;
            return x != y ? x < y : a < b;
        }
        const auto& x = p.value(a); const auto& y = p.value(b);
        if (x.spillable != y.spillable) return !x.spillable;
        if (spill_cost(x) != spill_cost(y)) return spill_cost(x) > spill_cost(y);
        if (p.neighbors(a).size() != p.neighbors(b).size()) return p.neighbors(a).size() > p.neighbors(b).size();
        return a < b;
    };
    if (intervals) for (ValueId v = 0; v < p.size(); ++v)
        if (p.value(v).ranges.empty()) throw std::invalid_argument("regional scan requires live intervals");
    std::vector<ValueId> order;
    auto append = [&](std::vector<ValueId> group) {
        std::sort(group.begin(), group.end(), less);
        order.insert(order.end(), group.begin(), group.end());
    };
    for (auto i : priority) append(regions[i].values);
    std::vector<ValueId> rest;
    for (ValueId v = 0; v < p.size(); ++v) if (!seen[v]) rest.push_back(v);
    append(std::move(rest));
    return order;
}
} // namespace detail
} // namespace firestone::regalloc
