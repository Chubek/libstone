#pragma once

#include "MemoryLocation.hpp"
#include "../pointer-analysis/PointsTo.hpp"
#include <cstddef>

namespace wetstone::alias {

enum class AliasResult { NoAlias, MayAlias, MustAlias };

// Query for ranges already normalized to one identical dynamic base.
// Malformed endpoints are unknown, not empty or disjoint.
[[nodiscard]] AliasResult range_alias(const ByteRange& a, const ByteRange& b);

// Classify byte overlap, not pointer equality. MustAlias means definite
// nonempty overlap (ranges need not coincide); MayAlias includes missing
// evidence. A common ValueId may prove a range relation only when it denotes
// the same dynamic pointer value in both locations.
//
// Points-to facts must be a conservative overapproximation for both locations
// in the same object namespace, with distinct IDs denoting distinct storage.
// This API does not qualify a legacy PointerAnalyzer::build result as sound.
[[nodiscard]] AliasResult alias(const MemoryLocation& a, const MemoryLocation& b,
                                const pointer::PointsToResult& points_to = {});

// Flow-insensitive object provenance for reachable, verified Wetstone IR.
// Reuses Andersen's inclusion solver but gives unsupported producers an
// explicit unknown value. In particular, memory loads are not modeled.
// Invalid SSA and an exhausted solver budget produce incomplete results.
[[nodiscard]] pointer::PointsToResult
analyze_points_to(const ir::Function& function, std::size_t iteration_limit = 10000);

} // namespace wetstone::alias
