#pragma once
#include "Common.hpp"

namespace firestone::isel {
struct GraphCovering {
    Selection select(const Graph& graph, const Rules& rules, const std::vector<NodeId>& roots, std::size_t budget = 100000) const {
        return detail::exact(Problem(graph, rules, roots), budget);
    }
};
} // namespace firestone::isel
