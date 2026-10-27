#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct RegisterOperation { Operation operation; std::vector<std::size_t> reads, writes; };
inline Graph register_dependencies(const std::vector<RegisterOperation>& program) {
    Graph out; std::map<std::size_t, NodeId> writer; std::map<std::size_t, std::set<NodeId>> readers;
    for (const auto& inst : program) {
        auto id = out.add(inst.operation);
        for (auto r : inst.reads) {
            if (writer.count(r)) out.depend(writer.at(r), id, out.node(writer.at(r)).latency, DependencyKind::Data);
            readers[r].insert(id);
        }
        for (auto r : inst.writes) {
            if (writer.count(r)) out.depend(writer.at(r), id, out.node(writer.at(r)).latency, DependencyKind::Output);
            for (auto reader : readers[r]) if (reader != id) out.depend(reader, id, 0, DependencyKind::Anti);
            readers[r].clear(); writer[r] = id;
        }
    }
    return out;
}
struct Execution {
    Schedule execution;
    std::vector<Cycle> issue, complete, retire;
    Cycle cycles = 0;
};
namespace detail {
inline Graph renamed(const Graph& g) {
    Graph out; for (NodeId i = 0; i < g.size(); ++i) out.add(g.node(i));
    for (const auto& d : g.dependencies()) if (d.kind != DependencyKind::Anti && d.kind != DependencyKind::Output)
        out.depend(d.from, d.to, d.delay, d.kind, d.distance);
    return out;
}
inline Execution simulate(const Graph& original, const Machine& m, std::size_t window,
                          std::size_t writeback_width, bool rename, bool scoreboard) {
    validate(original, m);
    if (window == 0 || writeback_width == 0) throw std::invalid_argument("zero execution capacity");
    // The input IDs are program order for dynamic execution.
    for (const auto& d : original.dependencies()) if (d.from >= d.to) throw std::invalid_argument("dynamic execution requires program-order dependencies");
    auto g = rename ? renamed(original) : original;
    Execution out; out.execution.start.assign(g.size(), ir::invalid_id);
    out.issue.assign(g.size(), ir::invalid_id); out.complete = out.issue; out.retire = out.issue;
    Usage usage; std::map<Cycle, std::size_t> starts;
    std::size_t issued = 0, retired = 0; Cycle clock = 0;
    while (retired < g.size()) {
        std::size_t broadcasts = 0;
        for (NodeId id = 0; id < issued && broadcasts < writeback_width; ++id)
            if (out.execution.start[id] != ir::invalid_id && out.complete[id] == ir::invalid_id &&
                checked_add(out.execution.start[id], g.node(id).latency) <= clock) { out.complete[id] = clock; ++broadcasts; }
        std::size_t commits = 0;
        while (retired < issued && out.complete[retired] != ir::invalid_id && commits < m.issue_width) {
            out.retire[retired] = clock; ++retired; ++commits;
        }
        for (std::size_t n = 0; n < m.issue_width && issued < g.size() && issued - retired < window; ++n) {
            bool blocked = false;
            if (scoreboard) {
                std::size_t busy = 0;
                for (NodeId id = 0; id < issued; ++id) if (out.complete[id] == ir::invalid_id && g.node(id).resource == g.node(issued).resource)
                    busy = checked_add(busy, g.node(id).units);
                blocked = busy > m.resources.at(g.node(issued).resource) - g.node(issued).units;
                for (const auto& d : g.dependencies()) if (d.to == issued && d.kind == DependencyKind::Output && out.complete[d.from] == ir::invalid_id) blocked = true;
            }
            if (blocked) break;
            out.issue[issued] = clock; ++issued;
        }
        for (NodeId id = 0; id < issued; ++id) if (out.execution.start[id] == ir::invalid_id) {
            bool ready = true;
            for (const auto& d : g.dependencies()) if (d.to == id) {
                if (out.execution.start[d.from] == ir::invalid_id) { ready = false; break; }
                if (checked_add(out.execution.start[d.from], d.delay) > clock) ready = false;
                if (d.kind == DependencyKind::Data && out.complete[d.from] == ir::invalid_id) ready = false;
            }
            if (ready && fits(g.node(id), clock, m, usage, starts)) {
                out.execution.start[id] = clock; reserve(g.node(id), clock, usage, starts, true);
                out.execution.makespan = std::max(out.execution.makespan, checked_add(clock, g.node(id).latency));
            }
        }
        out.cycles = clock;
        if (retired < g.size()) clock = checked_add(clock, 1);
    }
    return out;
}
} // namespace detail
struct OutOfOrder {
    Execution run(const Graph& g, const Machine& m = {}, std::size_t window = 16,
                  std::size_t writeback_width = 1) const { return detail::simulate(g, m, window, writeback_width, true, false); }
};
} // namespace firestone::isched
