#pragma once

#include "MemoryLocation.hpp"
#include "MustMay.hpp"
#include <cstddef>
#include <optional>
#include <vector>

namespace wetstone::alias {

enum class Reachability { Unknown, Unreachable, Reachable };
enum class DependenceKind { Flow, Anti, Output };
enum class IterationRelation { Unknown, SameIteration, LoopCarried };

// A possible ordered memory conflict, not a claim of an executed path, an
// intervening-write-free flow, or a must-dependence. Self pairs refer to
// separate executions of the instruction.
struct Dependence {
    MemoryAccess source;
    MemoryAccess sink;
    DependenceKind kind;
    IterationRelation iteration = IterationRelation::Unknown;
    std::optional<std::size_t> distance;
    AliasResult alias = AliasResult::MayAlias;
};

struct DependenceResult {
    // An incomplete result (e.g. invalid CFG or unsupported loop shape)
    // cannot establish absence. Complete results can still contain may facts.
    bool complete = true;
    std::vector<Dependence> dependences;
};

namespace detail {
void append_dependences(DependenceResult&, const MemoryAccess&, const MemoryAccess&,
                        IterationRelation, AliasResult);
}

} // namespace wetstone::alias
