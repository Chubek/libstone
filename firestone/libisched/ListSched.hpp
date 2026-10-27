#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct ListSched {
    Schedule schedule(const Graph& g, const Machine& m = {}) const {
        auto h = detail::heights(g); return detail::list(g, m, {h.begin(), h.end()});
    }
};
} // namespace firestone::isched
