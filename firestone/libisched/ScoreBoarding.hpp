#pragma once
#include "OutOfOrder.hpp"
namespace firestone::isched {
struct ScoreBoarding {
    Execution run(const Graph& g, const Machine& m = {}) const {
        return detail::simulate(g, m, std::max<std::size_t>(1, g.size()), m.issue_width, false, true);
    }
};
} // namespace firestone::isched
