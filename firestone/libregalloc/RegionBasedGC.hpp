#pragma once
#include "GreedyRegalloc.hpp"
#include "Regions.hpp"

namespace firestone::regalloc {
// Hot regions color first; global conflicts, precolors and aliases remain active.
struct RegionBasedGC {
    Allocation allocate(const Problem& p, const Machine& m,
                        const std::vector<AllocationRegion>& regions = {},
                        GreedyOptions options = {}) const {
        auto order = detail::region_order(p, regions, false);
        return detail::greedy(p, m, options, std::move(order));
    }
};
} // namespace firestone::regalloc
