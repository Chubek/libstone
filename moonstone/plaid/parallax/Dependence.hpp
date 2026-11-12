#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace moonstone::plaid::parallax {
// Simplified affine byte accesses: address(i)=base+offset+stride*i.
// Conservative algorithm returns Unknown if iteration pair budget is exceeded
// or signed address arithmetic would overflow. No alias proof from names alone.
struct MemoryAccess {
    std::uint64_t allocation_id = 0; // same ID means same known allocation
    std::int64_t offset = 0, stride = 0;
    std::uint64_t width = 1;
    bool writes = false;
};
enum class Dependence { Independent, Dependent, Unknown };
struct DependenceResult { Dependence status; std::string reason; };
inline DependenceResult loop_dependence(
    const std::vector<MemoryAccess>& accesses, std::size_t iterations,
    std::size_t pair_budget = 100000) {
    if (!pair_budget) return {Dependence::Unknown,"analysis budget exhausted"};
    for (const auto& access:accesses)
        if (!access.width) throw std::invalid_argument("zero memory access width");
    if (iterations > 1 && iterations > pair_budget / iterations)
        return {Dependence::Unknown,"pair enumeration exceeds budget"};
    auto byte_range = [](const MemoryAccess& a,std::size_t i,std::int64_t& first,std::int64_t& last) {
        const auto max = std::numeric_limits<std::int64_t>::max();
        const auto min = std::numeric_limits<std::int64_t>::min();
        if (i > static_cast<std::size_t>(max)) return false;
        const auto n = static_cast<std::uint64_t>(i);
        const auto mag = a.stride >= 0 ? static_cast<std::uint64_t>(a.stride)
                       : static_cast<std::uint64_t>(-(a.stride + 1)) + 1;
        if (mag && n > (static_cast<std::uint64_t>(max) + (a.stride < 0 ? 1u : 0u)) / mag) return false;
        const auto product = n*mag;
        std::int64_t shift;
        if (a.stride >= 0) shift = static_cast<std::int64_t>(product);
        else if (product == static_cast<std::uint64_t>(max)+1) shift = min;
        else shift = -static_cast<std::int64_t>(product);
        if ((shift > 0 && a.offset > max-shift) ||
            (shift < 0 && a.offset < min-shift)) return false;
        first = a.offset+shift;
        const auto available = static_cast<std::uint64_t>(max) - static_cast<std::uint64_t>(first);
        if (a.width-1 > available) return false;
        last = first + static_cast<std::int64_t>(a.width-1);
        return true;
    };
    for (std::size_t i=0;i<iterations;++i)
        for (std::size_t j=i+1;j<iterations;++j)
            for (const auto& a:accesses)
                for (const auto& b:accesses) {
                    if (!a.writes && !b.writes) continue;
                    if (a.allocation_id != b.allocation_id) continue;
                    std::int64_t ai,az,bi,bz;
                    if (!byte_range(a,i,ai,az) || !byte_range(b,j,bi,bz))
                        return {Dependence::Unknown,"address arithmetic out of range"};
                    if (ai<=bz && bi<=az)
                        return {Dependence::Dependent,"cross-iteration overlapping write"};
                    if (!byte_range(b,i,ai,az) || !byte_range(a,j,bi,bz))
                        return {Dependence::Unknown,"address arithmetic out of range"};
                    if (ai<=bz && bi<=az)
                        return {Dependence::Dependent,"cross-iteration overlapping write"};
                }
    return {Dependence::Independent,"all enumerated cross-iteration accesses disjoint"};
}
} // namespace moonstone::plaid::parallax
