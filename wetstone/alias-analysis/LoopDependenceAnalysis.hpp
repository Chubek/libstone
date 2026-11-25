#pragma once

#include "DirectAnalysis.hpp"
#include "../path-analysis/Loops.hpp"
#include <span>

namespace wetstone::alias {

// Analyze one natural loop invocation, classifying paths by whether they
// traverse a backedge to its header. Paths leaving the loop are not paired
// across distinct invocations. Other inner cycles may repeat within one
// iteration of the requested loop. Positive distances are not determined.
class LoopDependenceAnalysis {
public:
    [[nodiscard]] static DependenceResult analyze(const DirectAnalysis&, const path::Loop&);

    // Analyze only the supplied inventory (e.g. accesses with refined byte
    // extents). The caller is responsible for its sizes, offsets, effects, and
    // coverage; retain opaque calls/effects when refining the default inventory.
    [[nodiscard]] static DependenceResult analyze(const DirectAnalysis&, const path::Loop&,
                                                   std::span<const MemoryAccess>);
};

[[nodiscard]] DependenceResult analyze_loop_dependences(const DirectAnalysis&, const path::Loop&);
[[nodiscard]] DependenceResult analyze_loop_dependences(const DirectAnalysis&, const path::Loop&,
                                                        std::span<const MemoryAccess>);

} // namespace wetstone::alias
