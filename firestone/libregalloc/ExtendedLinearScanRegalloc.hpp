#pragma once
#include "LinearScanRegalloc.hpp"
namespace firestone::regalloc {
struct ExtendedLinearScanRegalloc {
    Allocation allocate(const Problem& p, const Machine& m) const { return LinearScanRegalloc{}.allocate(p, m, {true, true}); }
};
} // namespace firestone::regalloc
