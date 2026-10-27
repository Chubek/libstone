#pragma once
#include "ExtendedLinearScanRegalloc.hpp"
#include "Liveness.hpp"
namespace firestone::regalloc {
struct SSABasedLS : ExtendedLinearScanRegalloc {
    using ExtendedLinearScanRegalloc::allocate;
    Allocation allocate(const ir::SimpleSSAGraph& ssa, const Machine& m) const {
        return ExtendedLinearScanRegalloc::allocate(from_ssa(ssa), m);
    }
};
}
