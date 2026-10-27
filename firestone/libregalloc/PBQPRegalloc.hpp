#pragma once
#include "GreedyRegalloc.hpp"

namespace firestone::regalloc {
struct PBQPEdge {
    ValueId a = 0, b = 0;
    std::vector<std::vector<double>> costs; // Infinity forbids a pair.
};
struct PBQPModel {
    std::vector<std::vector<Location>> domains;
    std::vector<std::vector<double>> costs;
    std::vector<PBQPEdge> edges;
};
struct PBQPRegalloc {
    PBQPModel formulate(const Problem& p, const Machine& m) const {
        validate(p, m); PBQPModel out;
        for (ValueId v = 0; v < p.size(); ++v) {
            std::vector<Location> domain; std::vector<double> costs;
            for (auto r : candidates(p.value(v), m)) { domain.push_back(Location::reg(r)); costs.push_back(m.registers[r].cost); }
            auto spill = detail::spill(p.value(v));
            if (spill.kind != LocationKind::Unassigned) { domain.push_back(spill); costs.push_back(detail::spill_cost(p.value(v))); }
            out.domains.push_back(std::move(domain)); out.costs.push_back(std::move(costs));
        }
        for (ValueId a = 0; a < p.size(); ++a) for (ValueId b = a + 1; b < p.size(); ++b) {
            double affinity = 0;
            for (auto x : p.affinities()) if ((x.a == a && x.b == b) || (x.a == b && x.b == a)) affinity = detail::add_cost(affinity, x.weight);
            if (!p.interferes(a, b) && affinity == 0) continue;
            PBQPEdge edge; edge.a = a; edge.b = b;
            for (auto x : out.domains[a]) {
                std::vector<double> row;
                for (auto y : out.domains[b]) {
                    bool reg = x.kind == LocationKind::Register && y.kind == LocationKind::Register;
                    row.push_back(reg && p.interferes(a, b) && m.aliases(x.index, y.index) ?
                        std::numeric_limits<double>::infinity() : reg && x.index == y.index ? 0 : affinity);
                }
                edge.costs.push_back(std::move(row));
            }
            out.edges.push_back(std::move(edge));
        }
        return out;
    }
    // Exact finite-domain branch-and-bound over the PBQP model; bounded for JIT callers.
    Allocation allocate(const Problem& p, const Machine& m, std::size_t budget = 100000) const {
        auto model = formulate(p, m); auto best = GreedyRegalloc{}.allocate(p, m);
        double upper = best.feasible ? best.cost : std::numeric_limits<double>::infinity();
        std::vector<std::size_t> selected(p.size(), invalid_id); std::size_t explored = 0; bool limited = false;
        auto incremental = [&](ValueId v, std::size_t choice) {
            double cost = model.costs[v][choice];
            for (const auto& e : model.edges) {
                double term = 0;
                if (e.a == v && selected[e.b] != invalid_id) term = e.costs[choice][selected[e.b]];
                if (e.b == v && selected[e.a] != invalid_id) term = e.costs[selected[e.a]][choice];
                if (!std::isfinite(term)) return term;
                cost = detail::add_cost(cost, term);
            }
            return cost;
        };
        std::function<void(std::size_t, double)> search = [&](std::size_t count, double cost) {
            if (explored >= budget) { limited = true; return; }
            ++explored;
            if (count == p.size()) {
                if (!best.feasible || cost < upper) {
                    best = {}; best.locations.resize(p.size());
                    for (ValueId v = 0; v < p.size(); ++v) best.locations[v] = model.domains[v][selected[v]];
                    detail::finish(p, m, best); upper = best.cost;
                }
                return;
            }
            ValueId next = invalid_id; std::vector<std::pair<double, std::size_t>> choices;
            double lower = cost;
            for (ValueId v = 0; v < p.size(); ++v) if (selected[v] == invalid_id) {
                std::vector<std::pair<double, std::size_t>> available;
                for (std::size_t c = 0; c < model.domains[v].size(); ++c) {
                    auto delta = incremental(v, c);
                    if (std::isfinite(delta)) available.emplace_back(delta, c);
                }
                if (available.empty()) return;
                std::sort(available.begin(), available.end()); lower = detail::add_cost(lower, available.front().first);
                if (next == invalid_id || available.size() < choices.size()) { next = v; choices = std::move(available); }
            }
            // Remaining pair costs are nonnegative, so ignoring them is a valid lower bound.
            if (best.feasible && lower >= upper) return;
            for (auto c : choices) {
                selected[next] = c.second; search(count + 1, detail::add_cost(cost, c.first)); selected[next] = invalid_id;
                if (limited) return;
            }
        };
        search(0, 0);
        best.explored = explored; best.optimal = !limited && best.feasible;
        best.stop = limited ? Stop::SearchLimit : best.feasible ? Stop::Complete : Stop::Infeasible;
        return best;
    }
};
} // namespace firestone::regalloc
