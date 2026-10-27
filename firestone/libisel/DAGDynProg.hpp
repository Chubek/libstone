#pragma once
#include "Common.hpp"

namespace firestone::isel {
struct DAGDynProg {
    Selection select(const Graph& graph, const Rules& rules, const std::vector<NodeId>& roots) const {
        return detail::dynamic(Problem(graph, rules, roots), false, true);
    }
};
} // namespace firestone::isel
