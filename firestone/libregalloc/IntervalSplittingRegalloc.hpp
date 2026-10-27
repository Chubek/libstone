#pragma once
#include "LiveRangeSplitting.hpp"
#include "ExtendedLinearScanRegalloc.hpp"
namespace firestone::regalloc {
struct SplitTransfer {
    ValueId value = 0;
    Position position = 0;
    Location from, to;
};
struct SplitAllocation {
    SplitProblem split;
    Allocation allocation;
    std::vector<SplitTransfer> transfers;
};
struct IntervalSplittingRegalloc {
    SplitAllocation allocate(const Problem& p, const Machine& m,
                             const std::map<ValueId, std::vector<Position>>& cuts) const {
        SplitAllocation out; out.split = LiveRangeSplitting{}.split(p, cuts);
        out.allocation = ExtendedLinearScanRegalloc{}.allocate(out.split.problem, m);
        if (out.allocation.feasible) for (auto b : out.split.boundaries) {
            auto from = out.allocation.locations[b.before], to = out.allocation.locations[b.after];
            if (from != to) out.transfers.push_back({out.split.original[b.before], b.position, from, to});
        }
        return out;
    }
};
} // namespace firestone::regalloc
