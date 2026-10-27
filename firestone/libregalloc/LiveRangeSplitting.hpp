#pragma once
#include "Common.hpp"

namespace firestone::regalloc {
struct SplitBoundary {
    ValueId before = 0, after = 0; // Fragment IDs.
    Position position = 0;
};
struct SplitProblem {
    Problem problem;
    std::vector<ValueId> original; // Fragment ID -> original value.
    std::vector<std::vector<ValueId>> fragments;
    std::vector<SplitBoundary> boundaries;
};
struct LiveRangeSplitting {
    SplitProblem split(const Problem& p, const std::map<ValueId, std::vector<Position>>& cuts) const {
        for (const auto& item : cuts) {
            p.value(item.first);
            if (!std::is_sorted(item.second.begin(), item.second.end()) ||
                std::adjacent_find(item.second.begin(), item.second.end()) != item.second.end())
                throw std::invalid_argument("split points must be sorted and unique");
        }
        SplitProblem out; out.fragments.resize(p.size());
        for (ValueId v = 0; v < p.size(); ++v) {
            const auto& source = p.value(v); auto found = cuts.find(v);
            if (found == cuts.end() || found->second.empty()) {
                auto id = out.problem.add(source); out.original.push_back(v); out.fragments[v].push_back(id); continue;
            }
            if (source.ranges.empty()) throw std::invalid_argument("cannot split a graph-only value");
            std::vector<Position> limits{source.ranges.front().begin};
            for (auto cut : found->second) {
                if (cut <= limits.front() || cut >= source.ranges.back().end) throw std::invalid_argument("split outside live interval");
                limits.push_back(cut);
            }
            limits.push_back(source.ranges.back().end);
            double total = 0;
            for (auto r : source.ranges) total += static_cast<double>(r.end - r.begin);
            for (std::size_t i = 1; i < limits.size(); ++i) {
                Value fragment = source; fragment.ranges.clear(); fragment.uses.clear(); double length = 0;
                for (auto r : source.ranges) {
                    LiveRange part{std::max(r.begin, limits[i-1]), std::min(r.end, limits[i])};
                    if (part.begin < part.end) { fragment.ranges.push_back(part); length += static_cast<double>(part.end - part.begin); }
                }
                if (fragment.ranges.empty()) continue;
                for (auto use : source.uses) if (limits[i-1] <= use && use < limits[i]) fragment.uses.push_back(use);
                fragment.spill_cost *= length / total;
                if (fragment.rematerialization_cost) *fragment.rematerialization_cost *= length / total;
                auto id = out.problem.add(std::move(fragment)); out.original.push_back(v);
                if (!out.fragments[v].empty()) out.boundaries.push_back({out.fragments[v].back(), id, limits[i-1]});
                out.fragments[v].push_back(id);
            }
        }
        // Explicit graph edges have no position information, so retain them conservatively.
        for (ValueId v = 0; v < p.size(); ++v) for (auto n : p.explicit_neighbors(v)) if (n > v)
            for (auto a : out.fragments[v]) for (auto b : out.fragments[n]) out.problem.interfere(a, b);
        for (auto affinity : p.affinities()) {
            auto weight = affinity.weight / static_cast<double>(out.fragments[affinity.a].size()) /
                          static_cast<double>(out.fragments[affinity.b].size());
            for (auto a : out.fragments[affinity.a]) for (auto b : out.fragments[affinity.b]) out.problem.prefer(a, b, weight);
        }
        return out;
    }
};
} // namespace firestone::regalloc
