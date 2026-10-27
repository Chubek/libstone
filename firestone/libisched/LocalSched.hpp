#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct LocalSched {
    Schedule schedule(const Graph& g, const Machine& m = {}) const {
        return detail::list(g, m, std::vector<double>(g.size(), 0));
    }
};
} // namespace firestone::isched
