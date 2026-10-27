#pragma once
#include "GreedyRegalloc.hpp"
#include <random>

namespace firestone::regalloc {
struct SimulatedAnnealingRegalloc {
    Allocation allocate(const Problem& p, const Machine& m, std::size_t iterations = 10000,
                        std::uint32_t seed = 0, double temperature = 1) const {
        if (!std::isfinite(temperature) || temperature <= 0) throw std::invalid_argument("invalid annealing temperature");
        auto best = GreedyRegalloc{}.allocate(p, m);
        if (!best.feasible || p.size() == 0) return best;
        auto current = best; std::mt19937 random(seed);
        std::uniform_real_distribution<double> unit(0, 1);
        for (std::size_t i = 0; i < iterations; ++i) {
            auto v = std::uniform_int_distribution<ValueId>(0, p.size() - 1)(random);
            if (p.value(v).fixed) continue;
            auto proposal = current; auto regs = candidates(p.value(v), m);
            auto choice = std::uniform_int_distribution<std::size_t>(0, regs.size())(random);
            auto location = choice == regs.size() ? detail::spill(p.value(v)) : Location::reg(regs[choice]);
            if (location.kind == LocationKind::Unassigned) continue;
            bool valid = true;
            if (location.kind == LocationKind::Register) for (auto n : p.neighbors(v))
                if (proposal.locations[n].kind == LocationKind::Register && m.aliases(location.index, proposal.locations[n].index)) {
                    auto spill = detail::spill(p.value(n));
                    if (spill.kind == LocationKind::Unassigned) { valid = false; break; }
                    proposal.locations[n] = spill;
                }
            if (!valid) continue;
            proposal.locations[v] = location; detail::finish(p, m, proposal);
            auto heat = temperature * (1.0 - static_cast<double>(i) / static_cast<double>(iterations));
            auto delta = proposal.cost - current.cost;
            if (delta <= 0 || (heat > 0 && unit(random) < std::exp(-delta / heat))) current = std::move(proposal);
            if (current.cost < best.cost) best = current;
        }
        best.explored = iterations; return best;
    }
};
} // namespace firestone::regalloc
