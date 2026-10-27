#pragma once
#include "Common.hpp"
namespace firestone::regalloc {
struct SpillSlotColoring {
    Allocation color(const Problem& p, const Machine& m, Allocation allocation) const {
        if (!verify(p, m, allocation).empty()) throw std::invalid_argument("spill-slot coloring requires a valid allocation");
        detail::finish(p, m, allocation); return allocation;
    }
};
} // namespace firestone::regalloc
