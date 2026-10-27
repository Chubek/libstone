#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct ConstraintProgSched {
    Schedule schedule(const Graph& g, const Machine& m = {}, std::size_t budget = 100000) const {
        return detail::exact(g, m, budget, false);
    }
};
} // namespace firestone::isched
