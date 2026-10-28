#pragma once
#include "RegionBasedLS.hpp"
#include "IntervalSplittingRegalloc.hpp"

namespace firestone::regalloc {
struct TraceBasedLS {
    // Caller supplies trace membership and profile frequencies. A value shared
    // across traces must be listed only once; its global lifetime is retained.
    Allocation allocate(const Problem& p, const Machine& m,
                        const std::vector<AllocationRegion>& traces = {},
                        LinearScanOptions options = {true, true}) const {
        return RegionBasedLS{}.allocate(p, m, traces, options);
    }
    // Explicit trace-boundary cuts produce independent fragments and the exact
    // location transfers the backend must resolve at those boundaries.
    SplitAllocation allocate_split(const Problem& p, const Machine& m,
                                  const std::map<ValueId, std::vector<Position>>& cuts,
                                  const std::vector<AllocationRegion>& traces = {}) const {
        SplitAllocation out;
        out.split = LiveRangeSplitting{}.split(p, cuts);
        // Validate membership in the original value-ID space before expanding.
        detail::region_order(p, traces, true);
        std::vector<AllocationRegion> fragments;
        for (const auto& trace : traces) {
            AllocationRegion region; region.frequency = trace.frequency;
            for (auto v : trace.values) {
                const auto& ids = out.split.fragments[v];
                region.values.insert(region.values.end(), ids.begin(), ids.end());
            }
            fragments.push_back(std::move(region));
        }
        out.allocation = RegionBasedLS{}.allocate(out.split.problem, m, fragments);
        if (out.allocation.feasible) for (auto b : out.split.boundaries) {
            auto from = out.allocation.locations[b.before], to = out.allocation.locations[b.after];
            if (from != to) out.transfers.push_back({out.split.original[b.before], b.position, from, to});
        }
        return out;
    }
};
} // namespace firestone::regalloc
