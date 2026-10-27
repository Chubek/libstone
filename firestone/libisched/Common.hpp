#pragma once
#include "../libisel/Common.hpp"
#include "../libir/SimpleSSAGraph.hpp"
#include <numeric>

namespace firestone::isched {
using NodeId = ir::NodeId;
using Cycle = std::size_t;
inline Cycle checked_add(Cycle a, Cycle b) {
    if (b > std::numeric_limits<Cycle>::max() - a) throw std::overflow_error("schedule time overflow");
    return a + b;
}
enum class DependencyKind { Data, Anti, Output, Memory, Control };
struct Dependency {
    NodeId from = 0, to = 0;
    Cycle delay = 1;
    std::size_t distance = 0; // Loop iteration distance, zero for ordinary DAGs.
    DependencyKind kind = DependencyKind::Data;
};
struct Operation {
    std::string opcode;
    Cycle latency = 1;
    std::string resource = "alu";
    Cycle occupancy = 1; // Initiation occupancy, independent of result latency.
    std::size_t units = 1;
    ir::BlockId block = 0;
    double frequency = 1;
    bool speculatable = false; // Caller guarantees no traps or observable effects.
    std::string predicate;
    std::optional<NodeId> source;
};
class Graph {
public:
    NodeId add(Operation op) {
        if (op.opcode.empty() || op.resource.empty() || op.latency == 0 || op.occupancy == 0 || op.units == 0 ||
            !std::isfinite(op.frequency) || op.frequency < 0) throw std::invalid_argument("invalid scheduling operation");
        operations_.push_back(std::move(op)); return operations_.size() - 1;
    }
    void depend(NodeId from, NodeId to, Cycle delay, DependencyKind kind = DependencyKind::Data, std::size_t distance = 0) {
        node(from); node(to);
        if (from == to && distance == 0) throw std::invalid_argument("self dependency");
        dependencies_.push_back({from, to, delay, distance, kind});
    }
    const Operation& node(NodeId id) const { return operations_.at(id); }
    std::size_t size() const { return operations_.size(); }
    const std::vector<Dependency>& dependencies() const { return dependencies_; }
    std::vector<NodeId> topological_order() const {
        ir::DirectedGraph<int> dag;
        for (std::size_t i = 0; i < size(); ++i) dag.add_node(0);
        for (const auto& d : dependencies_) if (d.distance == 0) dag.add_edge(d.from, d.to);
        return dag.topological_order();
    }
private:
    std::vector<Operation> operations_;
    std::vector<Dependency> dependencies_;
};
struct Machine {
    std::size_t issue_width = 1;
    std::map<std::string, std::size_t> resources{{"alu", 1}};
};
enum class Stop { Complete, Infeasible, SearchLimit };
struct Schedule {
    std::vector<Cycle> start; // Indexed by operation ID; invalid_id means unscheduled.
    Cycle makespan = 0;
    Stop stop = Stop::Complete;
    bool optimal = false;
    std::size_t explored = 0;
    std::vector<NodeId> order() const {
        std::vector<NodeId> ids;
        for (NodeId i = 0; i < start.size(); ++i) if (start[i] != ir::invalid_id) ids.push_back(i);
        std::stable_sort(ids.begin(), ids.end(), [&](auto a, auto b) { return start[a] < start[b]; }); return ids;
    }
};
inline void validate(const Graph& g, const Machine& m, bool loops = false) {
    if (m.issue_width == 0) throw std::invalid_argument("zero issue width");
    for (const auto& r : m.resources) if (r.first.empty() || r.second == 0) throw std::invalid_argument("invalid resource capacity");
    for (NodeId i = 0; i < g.size(); ++i) {
        const auto& op = g.node(i); auto r = m.resources.find(op.resource);
        if (r == m.resources.end() || op.units > r->second) throw std::invalid_argument("operation exceeds machine resources");
    }
    for (const auto& d : g.dependencies()) if (!loops && d.distance != 0)
        throw std::invalid_argument("loop-carried dependency requires modulo scheduling or unrolling");
    g.topological_order();
}
namespace detail {
using Usage = std::map<std::pair<Cycle, std::string>, std::size_t>;
inline bool fits(const Operation& op, Cycle cycle, const Machine& m, const Usage& usage,
                 const std::map<Cycle, std::size_t>& issue) {
    auto it = issue.find(cycle);
    if (it != issue.end() && it->second >= m.issue_width) return false;
    auto end = checked_add(cycle, op.occupancy);
    for (auto c = cycle; c < end; ++c) {
        auto u = usage.find({c, op.resource});
        if (u != usage.end() && u->second > m.resources.at(op.resource) - op.units) return false;
    }
    return true;
}
inline void reserve(const Operation& op, Cycle cycle, Usage& usage, std::map<Cycle, std::size_t>& issue, bool add) {
    if (add) ++issue[cycle]; else --issue[cycle];
    auto end = checked_add(cycle, op.occupancy);
    for (auto c = cycle; c < end; ++c) {
        if (add) usage[{c, op.resource}] += op.units; else usage[{c, op.resource}] -= op.units;
    }
}
inline std::vector<Cycle> heights(const Graph& g) {
    auto order = g.topological_order(); std::vector<Cycle> height(g.size());
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        height[*it] = g.node(*it).latency;
        for (const auto& d : g.dependencies()) if (d.from == *it && d.distance == 0)
            height[*it] = std::max(height[*it], checked_add(d.delay, height[d.to]));
    }
    return height;
}
inline Schedule list(const Graph& g, const Machine& m, const std::vector<double>& priority) {
    validate(g, m);
    if (priority.size() != g.size()) throw std::invalid_argument("priority size mismatch");
    for (auto p : priority) if (!std::isfinite(p)) throw std::invalid_argument("nonfinite priority");
    Schedule out; out.start.assign(g.size(), ir::invalid_id);
    Usage usage; std::map<Cycle, std::size_t> issue;
    for (std::size_t count = 0; count < g.size(); ++count) {
        NodeId chosen = ir::invalid_id; Cycle chosen_time = ir::invalid_id;
        for (NodeId id = 0; id < g.size(); ++id) if (out.start[id] == ir::invalid_id) {
            bool ready = true; Cycle earliest = 0;
            for (const auto& d : g.dependencies()) if (d.to == id) {
                if (out.start[d.from] == ir::invalid_id) { ready = false; break; }
                earliest = std::max(earliest, checked_add(out.start[d.from], d.delay));
            }
            if (!ready) continue;
            while (!fits(g.node(id), earliest, m, usage, issue)) earliest = checked_add(earliest, 1);
            if (chosen == ir::invalid_id || earliest < chosen_time ||
                (earliest == chosen_time && priority[id] > priority[chosen])) { chosen = id; chosen_time = earliest; }
        }
        if (chosen == ir::invalid_id) throw std::logic_error("unschedulable dependency graph");
        out.start[chosen] = chosen_time; reserve(g.node(chosen), chosen_time, usage, issue, true);
        out.makespan = std::max(out.makespan, checked_add(chosen_time, g.node(chosen).latency));
    }
    return out;
}
inline Schedule exact(const Graph& g, const Machine& m, std::size_t budget, bool time_indexed) {
    validate(g, m); auto height = heights(g);
    Schedule best = list(g, m, {height.begin(), height.end()});
    if (g.size() == 0) { best.optimal = true; return best; }
    Cycle lower = 0;
    for (auto h : height) lower = std::max(lower, h);
    std::size_t explored = 0; bool limit = false;
    // Search finite start-time domains at increasing makespan bounds.
    for (Cycle horizon = lower; horizon < best.makespan; ++horizon) {
        Schedule current; current.start.assign(g.size(), ir::invalid_id);
        Usage usage; std::map<Cycle, std::size_t> issue; bool found = false;
        std::function<void(std::size_t)> search = [&](std::size_t assigned) {
            if (explored >= budget) { limit = true; return; } ++explored;
            if (assigned == g.size()) { current.makespan = horizon; best = current; found = true; return; }
            NodeId next = ir::invalid_id; Cycle first = 0, last = 0, domain = ir::invalid_id;
            for (NodeId id = 0; id < g.size(); ++id) if (current.start[id] == ir::invalid_id) {
                Cycle earliest = 0; bool ready = true;
                for (const auto& d : g.dependencies()) if (d.to == id) {
                    if (current.start[d.from] == ir::invalid_id) { ready = false; break; }
                    earliest = std::max(earliest, checked_add(current.start[d.from], d.delay));
                }
                if (!ready) continue;
                if (height[id] > horizon || earliest > horizon - height[id]) return;
                auto latest = horizon - height[id];
                if (next == ir::invalid_id || (!time_indexed && latest - earliest < domain)) {
                    next = id; first = earliest; last = latest; domain = latest - earliest;
                }
            }
            if (next == ir::invalid_id) return;
            for (auto t = first; t <= last; ++t) if (fits(g.node(next), t, m, usage, issue)) {
                current.start[next] = t; reserve(g.node(next), t, usage, issue, true);
                search(assigned + 1);
                reserve(g.node(next), t, usage, issue, false); current.start[next] = ir::invalid_id;
                if (found || limit) return;
            }
        };
        search(0); if (found || limit) break;
    }
    best.explored = explored; best.optimal = !limit;
    if (limit) best.stop = Stop::SearchLimit;
    return best;
}
} // namespace detail
inline ir::Diagnostics verify(const Graph& g, const Machine& m, const Schedule& schedule) {
    validate(g, m); ir::Diagnostics errors;
    if (schedule.start.size() != g.size()) { ir::diagnose(errors, "size", "schedule size mismatch"); return errors; }
    detail::Usage usage; std::map<Cycle, std::size_t> issue; Cycle makespan = 0;
    for (NodeId id = 0; id < g.size(); ++id) {
        auto t = schedule.start[id];
        if (t == ir::invalid_id) { ir::diagnose(errors, "missing", "unscheduled operation", id); continue; }
        if (!detail::fits(g.node(id), t, m, usage, issue)) ir::diagnose(errors, "resource", "resource or issue-width conflict", id);
        detail::reserve(g.node(id), t, usage, issue, true);
        makespan = std::max(makespan, checked_add(t, g.node(id).latency));
    }
    for (const auto& d : g.dependencies()) if (schedule.start[d.from] != ir::invalid_id && schedule.start[d.to] != ir::invalid_id &&
        schedule.start[d.to] < checked_add(schedule.start[d.from], d.delay)) ir::diagnose(errors, "dependency", "dependency latency violated", d.to);
    if (schedule.makespan != makespan) ir::diagnose(errors, "makespan", "incorrect schedule makespan");
    return errors;
}
inline Graph from_selection(const isel::Selection& selection) {
    if (selection.stop == isel::Stop::Infeasible || (selection.instructions.empty() && !selection.roots.empty()))
        throw std::invalid_argument("selection has no feasible cover");
    Graph out; std::map<NodeId, NodeId> ids;
    for (const auto& inst : selection.instructions) {
        Operation op; op.opcode = inst.opcode; op.latency = inst.latency; op.resource = inst.resource; op.source = inst.result; op.occupancy = inst.occupancy; op.units = inst.units;
        if (ids.count(inst.result)) throw std::invalid_argument("duplicate selection result");
        auto id = out.add(op);
        for (auto input : inst.inputs) {
            auto found = ids.find(input);
            if (found == ids.end()) throw std::invalid_argument("selection input not defined before use");
            out.depend(found->second, id, out.node(found->second).latency);
        }
        ids[inst.result] = id;
    }
    for (auto root : selection.roots) if (!ids.count(root)) throw std::invalid_argument("selection output is not defined");
    return out;
}
// SSA block adapter preserves all effects in source order and pins terminators.
// Phi values are block-entry inputs, not schedulable instructions.
inline Graph from_ssa_block(const ir::SimpleSSAGraph& ssa, ir::BlockId block,
                           const std::function<Operation(const ir::Instruction&)>& target = {}) {
    auto errors = ssa.verify(); if (!errors.empty()) throw std::invalid_argument(errors.front().message);
    Graph out; std::map<ir::ValueId, NodeId> defs; std::optional<NodeId> effect;
    const auto& instructions = ssa.cfg().block(block).instructions;
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        const auto& inst = instructions[index]; if (inst.opcode == ir::Opcode::Phi) continue;
        Operation op;
        if (target) op = target(inst); else op.opcode = isel::opcode_name(inst.opcode);
        op.block = block; op.source = index; auto id = out.add(op);
        for (const auto& operand : inst.operands) if (operand.kind == ir::OperandKind::Value && defs.count(operand.value)) {
            auto from = defs.at(operand.value); out.depend(from, id, out.node(from).latency);
        }
        if (ir::is_effectful(inst.opcode) || inst.opcode == ir::Opcode::Div || inst.opcode == ir::Opcode::Mod) {
            if (effect) out.depend(*effect, id, out.node(*effect).latency, DependencyKind::Memory);
            effect = id;
        }
        if (ir::is_terminator(inst.opcode)) for (NodeId before = 0; before < id; ++before)
            out.depend(before, id, out.node(before).latency, DependencyKind::Control);
        if (inst.result) defs[*inst.result] = id;
    }
    return out;
}
} // namespace firestone::isched
