#pragma once
#include "OutOfOrder.hpp"
namespace firestone::isched {
struct Tomasulo {
    // Reservation stations retain tagged operands; renaming removes WAR/WAW.
    // The common data bus bounds completed results per cycle.
    Execution run(const Graph& g, const Machine& m = {}, std::size_t reservation_stations = 16,
                  std::size_t common_data_buses = 1) const {
        return detail::simulate(g, m, reservation_stations, common_data_buses, true, false);
    }
};
} // namespace firestone::isched
