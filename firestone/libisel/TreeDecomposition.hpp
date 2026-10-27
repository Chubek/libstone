#pragma once
#include "ILP.hpp"
namespace firestone::isel {
// Variable elimination on the primal graph of the exact-cover constraints.
// Each elimination bag contains one tile variable and its remaining neighbors.
struct TreeDecomposition {
    Selection select(const Graph& g, const Rules& r, const std::vector<NodeId>& roots,
                     std::size_t max_bag = 20) const {
        Problem p(g, r, roots);
        struct Entry { double cost = 0; std::vector<std::size_t> chosen; };
        struct Factor { std::vector<std::size_t> vars; std::vector<Entry> table; };
        const auto inf = std::numeric_limits<double>::infinity();
        if (max_bag >= std::numeric_limits<std::size_t>::digits) throw std::invalid_argument("bag bound too large");
        auto model = ILP{}.formulate(p); std::vector<Factor> factors;
        for (const auto& row : model.exactly_one) {
            if (row.second.empty()) return detail::failed(p);
            if (row.second.size() > max_bag) return detail::failed(p, Stop::SearchLimit);
            Factor f{row.second, std::vector<Entry>(std::size_t{1} << row.second.size())};
            for (std::size_t mask = 0; mask < f.table.size(); ++mask)
                if (mask == 0 || (mask & (mask - 1)) != 0) f.table[mask].cost = inf;
            factors.push_back(std::move(f));
        }
        for (std::size_t i = 0; i < p.tiles.size(); ++i) factors.push_back({{i}, {{0, {}}, {model.objective[i], {i}}}});
        std::set<std::size_t> remaining;
        for (std::size_t i = 0; i < p.tiles.size(); ++i) remaining.insert(i);
        while (!remaining.empty()) {
            std::size_t variable = 0; std::set<std::size_t> bag; std::size_t size = ir::invalid_id;
            for (auto v : remaining) {
                std::set<std::size_t> neighbors;
                for (const auto& f : factors) if (std::find(f.vars.begin(), f.vars.end(), v) != f.vars.end())
                    neighbors.insert(f.vars.begin(), f.vars.end());
                if (neighbors.size() < size) { size = neighbors.size(); variable = v; bag = std::move(neighbors); }
            }
            if (size > max_bag) return detail::failed(p, Stop::SearchLimit);
            std::vector<std::size_t> all(bag.begin(), bag.end()); bag.erase(variable);
            Factor result{{bag.begin(), bag.end()}, std::vector<Entry>(std::size_t{1} << bag.size(), {inf, {}})};
            std::vector<Factor> used, kept;
            for (auto& f : factors) {
                if (std::find(f.vars.begin(), f.vars.end(), variable) != f.vars.end()) used.push_back(std::move(f));
                else kept.push_back(std::move(f));
            }
            auto project = [&](std::size_t mask, const std::vector<std::size_t>& vars) {
                std::size_t out = 0;
                for (std::size_t i = 0; i < vars.size(); ++i) {
                    auto pos = static_cast<std::size_t>(std::lower_bound(all.begin(), all.end(), vars[i]) - all.begin());
                    if (mask & (std::size_t{1} << pos)) out |= std::size_t{1} << i;
                }
                return out;
            };
            for (std::size_t mask = 0; mask < (std::size_t{1} << all.size()); ++mask) {
                Entry e;
                for (const auto& f : used) {
                    const auto& value = f.table[project(mask, f.vars)]; e.cost += value.cost;
                    e.chosen.insert(e.chosen.end(), value.chosen.begin(), value.chosen.end());
                }
                auto& dest = result.table[project(mask, result.vars)];
                if (e.cost < dest.cost) dest = std::move(e);
            }
            kept.push_back(std::move(result)); factors = std::move(kept); remaining.erase(variable);
        }
        std::vector<std::size_t> chosen;
        for (const auto& f : factors) {
            if (!std::isfinite(f.table[0].cost)) return detail::failed(p);
            chosen.insert(chosen.end(), f.table[0].chosen.begin(), f.table[0].chosen.end());
        }
        auto out = materialize(p, chosen); out.optimal = true; return out;
    }
};
} // namespace firestone::isel
