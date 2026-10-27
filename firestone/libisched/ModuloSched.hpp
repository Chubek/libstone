#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct ModuloResult { Schedule kernel; Cycle initiation_interval = 0; };
inline bool modulo_dependencies(const Graph& g, const std::vector<Cycle>& start, Cycle ii) {
    for (const auto& d : g.dependencies()) {
        if (start[d.from] == ir::invalid_id || start[d.to] == ir::invalid_id) continue;
        if (d.distance && ii > std::numeric_limits<Cycle>::max() / d.distance) throw std::overflow_error("modulo distance overflow");
        if (checked_add(start[d.to], ii * d.distance) < checked_add(start[d.from], d.delay)) return false;
    }
    return true;
}
inline ir::Diagnostics verify(const Graph& g, const Machine& m, const ModuloResult& result) {
    validate(g, m, true); ir::Diagnostics errors;
    const auto ii = result.initiation_interval;
    if (!ii || result.kernel.start.size() != g.size()) { ir::diagnose(errors, "kernel", "invalid modulo kernel"); return errors; }
    detail::Usage usage; std::map<Cycle, std::size_t> issue;
    Cycle makespan = 0;
    for (NodeId id = 0; id < g.size(); ++id) {
        auto t = result.kernel.start[id]; const auto& op = g.node(id);
        if (t == ir::invalid_id) { ir::diagnose(errors, "missing", "unscheduled operation", id); continue; }
        if (++issue[t % ii] > m.issue_width) ir::diagnose(errors, "issue", "modulo issue conflict", id);
        for (Cycle offset = 0; offset < op.occupancy; ++offset) {
            auto& used = usage[{checked_add(t, offset) % ii, op.resource}]; used = checked_add(used, op.units);
            if (used > m.resources.at(op.resource)) ir::diagnose(errors, "resource", "modulo resource conflict", id);
        }
        makespan = std::max(makespan, checked_add(t, op.latency));
    }
    if (!modulo_dependencies(g, result.kernel.start, ii)) ir::diagnose(errors, "dependency", "loop dependency violated");
    if (result.kernel.makespan != makespan) ir::diagnose(errors, "makespan", "incorrect kernel makespan");
    return errors;
}
struct ModuloSched {
    // Finite-domain modulo scheduling. SearchLimit never means infeasible.
    ModuloResult schedule(const Graph& g, const Machine& m, Cycle max_ii = 64,
                          Cycle max_stage = 8, std::size_t budget = 100000) const {
        validate(g, m, true);
        if (max_ii == 0 || max_stage == 0) throw std::invalid_argument("zero modulo search bound");
        ModuloResult out; out.kernel.start.assign(g.size(), ir::invalid_id); out.kernel.stop = Stop::SearchLimit;
        auto order = g.topological_order(); std::size_t explored = 0; bool limited = false;
        for (Cycle ii = 1; ii <= max_ii; ++ii) {
            if (ii > std::numeric_limits<Cycle>::max() / max_stage) throw std::overflow_error("modulo horizon overflow");
            detail::Usage usage; std::map<Cycle, std::size_t> issue; bool found = false;
            std::function<void(std::size_t)> search = [&](std::size_t index) {
                if (explored >= budget) { limited = true; return; } ++explored;
                if (index == order.size()) { found = true; return; }
                auto id = order[index]; const auto& op = g.node(id);
                for (Cycle t = 0; t < ii * max_stage; ++t) {
                    if (issue[t % ii] >= m.issue_width) continue;
                    std::map<Cycle, std::size_t> need;
                    for (Cycle offset = 0; offset < op.occupancy; ++offset) {
                        auto& count = need[checked_add(t, offset) % ii]; count = checked_add(count, op.units);
                    }
                    bool fits = true;
                    for (const auto& n : need) if (n.second > m.resources.at(op.resource) ||
                        usage[{n.first, op.resource}] > m.resources.at(op.resource) - n.second) fits = false;
                    if (!fits) continue;
                    out.kernel.start[id] = t;
                    if (modulo_dependencies(g, out.kernel.start, ii)) {
                        ++issue[t % ii]; for (const auto& n : need) usage[{n.first, op.resource}] += n.second;
                        search(index + 1);
                        if (found) return;
                        --issue[t % ii]; for (const auto& n : need) usage[{n.first, op.resource}] -= n.second;
                    }
                    out.kernel.start[id] = ir::invalid_id;
                    if (limited) return;
                }
            };
            search(0);
            if (found) {
                out.initiation_interval = ii; out.kernel.stop = Stop::Complete;
                for (NodeId id = 0; id < g.size(); ++id) out.kernel.makespan = std::max(out.kernel.makespan, checked_add(out.kernel.start[id], g.node(id).latency));
                // Minimal only within the specified stage domain.
                out.kernel.explored = explored; return out;
            }
            if (limited) break;
        }
        out.kernel.explored = explored; return out;
    }
};
} // namespace firestone::isched
