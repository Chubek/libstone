#pragma once
#include "CoalescingRegalloc.hpp"

namespace firestone::regalloc {
// Conservative George (precolored endpoints) / Briggs (other endpoints)
// coalescing followed by optimistic simplify/select. This is a batch heuristic,
// not the full iterated worklist algorithm from George and Appel.
struct GeorgeAppelGC {
    CoalescingResult contract(const Problem& p, const Machine& m) const {
        return coalesce(p, m, false, true);
    }
    Allocation allocate(const Problem& p, const Machine& m, ColoringOptions options = {}) const {
        auto contracted = contract(p, m);
        auto colored = GraphColoringRegalloc{}.allocate(contracted.problem, m, options);
        auto out = detail::initial(p);
        for (ValueId v = 0; v < p.size(); ++v)
            out.locations[v] = colored.locations[contracted.representative[v]];
        detail::finish(p, m, out);
        return out;
    }
};
} // namespace firestone::regalloc
