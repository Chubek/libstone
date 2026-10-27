#pragma once
#include "ListSched.hpp"
#include <random>
namespace firestone::isched {
struct AntColonyOpt {
    Schedule schedule(const Graph& g, const Machine& m = {}, std::size_t iterations = 30,
                      std::size_t ants = 8, std::uint64_t seed = 0) const {
        if (ants == 0) throw std::invalid_argument("ant count must be positive");
        auto best = ListSched{}.schedule(g, m); auto h = detail::heights(g);
        std::vector<double> pheromone(g.size(), 1); std::mt19937_64 random(seed);
        std::uniform_real_distribution<double> uniform(0.000001, 1);
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            for (std::size_t ant = 0; ant < ants; ++ant) {
                std::vector<double> priorities(g.size());
                for (NodeId id = 0; id < g.size(); ++id)
                    priorities[id] = pheromone[id] * (1 + static_cast<double>(h[id])) / -std::log(uniform(random));
                auto candidate = detail::list(g, m, priorities);
                if (candidate.makespan < best.makespan) best = std::move(candidate);
            }
            for (auto& p : pheromone) p = std::max(0.01, p * 0.8);
            const auto order = best.order();
            for (std::size_t rank = 0; rank < order.size(); ++rank)
                pheromone[order[rank]] += 1.0 / ((1.0 + static_cast<double>(rank)) * (1.0 + static_cast<double>(best.makespan)));
        }
        return best;
    }
};
} // namespace firestone::isched
