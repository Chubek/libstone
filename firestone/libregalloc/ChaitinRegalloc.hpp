#pragma once
#include "GraphColoringRegalloc.hpp"
namespace firestone::regalloc {
struct ChaitinRegalloc {
    Allocation allocate(const Problem& p, const Machine& m) const { return GraphColoringRegalloc{}.allocate(p, m, {false, 0, 0}); }
};
} // namespace firestone::regalloc
