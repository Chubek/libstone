#pragma once
#include "CoalescingRegalloc.hpp"
namespace firestone::regalloc {
struct AggressiveGC {
    Allocation allocate(const Problem& p, const Machine& m) const { return CoalescingRegalloc{}.allocate(p, m, true); }
};
}
