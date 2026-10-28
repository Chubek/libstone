#pragma once
#include "LinearScanRegalloc.hpp"
#include "Regions.hpp"

namespace firestone::regalloc {
// Start-ordered within each hot-first region, with global active/inactive
// conflicts. Allocations are whole-value; regions do not imply live-range cuts.
struct RegionBasedLS {
    Allocation allocate(const Problem& p, const Machine& m,
                        const std::vector<AllocationRegion>& regions = {},
                        LinearScanOptions options = {true, true}) const {
        auto order = detail::region_order(p, regions, true);
        return detail::linear_scan(p, m, options, std::move(order));
    }
};
} // namespace firestone::regalloc
