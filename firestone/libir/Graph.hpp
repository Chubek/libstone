#pragma once

#include "Common.hpp"
#include <deque>
#include <iterator>
#include <sstream>

namespace firestone::ir {

enum class EdgeKind { Data, Control, Memory, Call, Interference, Value };
struct Edge {
    NodeId from = invalid_id;
    NodeId to = invalid_id;
    EdgeKind kind = EdgeKind::Data;
    std::size_t port = 0;
    friend bool operator==(const Edge& a, const Edge& b) {
        return a.from == b.from && a.to == b.to && a.kind == b.kind && a.port == b.port;
    }
};

// IDs are append-only. Erasure leaves a tombstone and never invalidates other IDs.
template<class Payload>
class DirectedGraph {
public:
    NodeId add_node(Payload payload = {}) {
        nodes_.emplace_back(std::move(payload));
        return nodes_.size() - 1;
    }
    bool contains(NodeId id) const { return id < nodes_.size() && nodes_[id].has_value(); }
    const Payload& node(NodeId id) const { check(id); return *nodes_[id]; }
    Payload& node(NodeId id) { check(id); return *nodes_[id]; }
    std::vector<NodeId> nodes() const {
        std::vector<NodeId> out;
        for (NodeId i = 0; i < nodes_.size(); ++i) if (contains(i)) out.push_back(i);
        return out;
    }
    std::size_t size() const {
        return static_cast<std::size_t>(std::count_if(nodes_.begin(), nodes_.end(), [](const auto& n) { return n.has_value(); }));
    }
    const std::vector<Edge>& edges() const { return edges_; }
    bool add_edge(NodeId from, NodeId to, EdgeKind kind = EdgeKind::Data, std::size_t port = 0) {
        check(from); check(to);
        Edge e{from, to, kind, port};
        if (std::find(edges_.begin(), edges_.end(), e) != edges_.end()) return false;
        edges_.push_back(e); return true;
    }
    void remove_edge(const Edge& edge) { edges_.erase(std::remove(edges_.begin(), edges_.end(), edge), edges_.end()); }
    void remove_node(NodeId id) {
        check(id); nodes_[id].reset();
        edges_.erase(std::remove_if(edges_.begin(), edges_.end(), [id](const Edge& e) { return e.from == id || e.to == id; }), edges_.end());
    }
    std::vector<NodeId> successors(NodeId id, std::optional<EdgeKind> kind = {}) const {
        return neighbors(id, false, kind);
    }
    std::vector<NodeId> predecessors(NodeId id, std::optional<EdgeKind> kind = {}) const {
        return neighbors(id, true, kind);
    }
    std::set<NodeId> reachable(NodeId start, bool reverse = false, std::optional<EdgeKind> kind = {}) const {
        check(start);
        auto adjacent = adjacency(reverse, kind);
        std::set<NodeId> seen{start};
        std::vector<NodeId> pending{start};
        while (!pending.empty()) {
            auto id = pending.back(); pending.pop_back();
            for (auto next : adjacent[id]) if (seen.insert(next).second) pending.push_back(next);
        }
        return seen;
    }
    std::vector<NodeId> shortest_path(NodeId start, NodeId finish, std::optional<EdgeKind> kind = {}) const {
        check(start); check(finish);
        if (start == finish) return {start};
        auto adjacent = adjacency(false, kind);
        std::map<NodeId, NodeId> parent{{start, start}};
        std::deque<NodeId> pending{start};
        while (!pending.empty() && !parent.count(finish)) {
            auto id = pending.front(); pending.pop_front();
            for (auto next : adjacent[id]) if (parent.emplace(next, id).second) pending.push_back(next);
        }
        if (!parent.count(finish)) return {};
        std::vector<NodeId> path{finish};
        while (path.back() != start) path.push_back(parent.at(path.back()));
        std::reverse(path.begin(), path.end()); return path;
    }
    std::vector<NodeId> reverse_postorder(NodeId start, bool reverse = false, std::optional<EdgeKind> kind = {}) const {
        check(start);
        auto adjacent = adjacency(reverse, kind);
        std::vector<bool> seen(nodes_.size());
        std::vector<NodeId> order;
        postorder(start, adjacent, seen, order);
        std::reverse(order.begin(), order.end()); return order;
    }
    std::vector<NodeId> topological_order() const {
        auto adjacent = adjacency(false);
        std::vector<std::size_t> degrees(nodes_.size());
        for (const auto& list : adjacent) for (auto to : list) ++degrees[to];
        std::set<NodeId> ready;
        auto live = nodes();
        for (auto id : live) if (degrees[id] == 0) ready.insert(id);
        std::vector<NodeId> order;
        while (!ready.empty()) {
            auto id = *ready.begin(); ready.erase(ready.begin()); order.push_back(id);
            for (auto next : adjacent[id]) if (--degrees[next] == 0) ready.insert(next);
        }
        if (order.size() != live.size()) throw std::logic_error("graph contains a cycle");
        return order;
    }
    std::vector<std::vector<NodeId>> strongly_connected_components() const {
        auto forward = adjacency(false);
        auto backward = adjacency(true);
        std::vector<bool> seen(nodes_.size());
        std::vector<NodeId> order;
        for (auto id : nodes()) if (!seen[id]) postorder(id, forward, seen, order);
        std::fill(seen.begin(), seen.end(), false);
        std::vector<std::vector<NodeId>> result;
        // Decreasing finish time separates the components in the transposed graph.
        for (auto id = order.rbegin(); id != order.rend(); ++id) if (!seen[*id]) {
            auto& component = result.emplace_back();
            postorder(*id, backward, seen, component);
            std::sort(component.begin(), component.end());
        }
        std::sort(result.begin(), result.end()); return result;
    }
    std::string to_dot(const std::function<std::string(const Payload&)>& label) const {
        auto escape = [](const std::string& s) {
            std::string out;
            for (char c : s) { if (c == '\\' || c == '"') out += '\\'; if (c == '\n') out += "\\n"; else out += c; }
            return out;
        };
        std::ostringstream out; out << "digraph firestone {\n";
        for (auto id : nodes()) out << "  n" << id << " [label=\"" << escape(label(node(id))) << "\"];\n";
        for (const auto& e : edges_) out << "  n" << e.from << " -> n" << e.to << " [label=\"" << static_cast<int>(e.kind) << ':' << e.port << "\"];\n";
        out << "}\n"; return out.str();
    }
private:
    std::vector<std::optional<Payload>> nodes_;
    std::vector<Edge> edges_;
    using Adjacency = std::vector<std::vector<NodeId>>;
    Adjacency adjacency(bool reverse, std::optional<EdgeKind> kind = {}) const {
        Adjacency result(nodes_.size());
        for (const auto& e : edges_) if (!kind || e.kind == *kind)
            result[reverse ? e.to : e.from].push_back(reverse ? e.from : e.to);
        for (auto& list : result) {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
        return result;
    }
    static void postorder(NodeId start, const Adjacency& adjacent, std::vector<bool>& seen, std::vector<NodeId>& order) {
        struct Frame { NodeId id; std::size_t next = 0; };
        std::vector<Frame> stack{{start}};
        seen[start] = true;
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.next == adjacent[frame.id].size()) { order.push_back(frame.id); stack.pop_back(); continue; }
            auto next = adjacent[frame.id][frame.next++];
            if (!seen[next]) { seen[next] = true; stack.push_back({next}); }
        }
    }
    void check(NodeId id) const { if (!contains(id)) throw std::out_of_range("invalid graph node"); }
    std::vector<NodeId> neighbors(NodeId id, bool reverse, std::optional<EdgeKind> kind) const {
        check(id); std::set<NodeId> out;
        for (const auto& e : edges_) if ((!kind || e.kind == *kind) && (reverse ? e.to : e.from) == id) out.insert(reverse ? e.from : e.to);
        return {out.begin(), out.end()};
    }
};

using DominatorSets = std::map<NodeId, std::set<NodeId>>;

template<class Payload>
DominatorSets compute_dominators(const DirectedGraph<Payload>& graph, NodeId entry, bool reverse = false) {
    const auto reachable = graph.reachable(entry, reverse);
    DominatorSets dom;
    for (auto n : graph.nodes()) dom[n] = reachable.count(n) ? reachable : std::set<NodeId>{n};
    dom[entry] = {entry};
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto n : reachable) {
            if (n == entry) continue;
            auto next = reachable;
            for (auto p : reverse ? graph.successors(n) : graph.predecessors(n)) {
                if (!reachable.count(p)) continue;
                std::set<NodeId> intersection;
                std::set_intersection(next.begin(), next.end(), dom[p].begin(), dom[p].end(), std::inserter(intersection, intersection.end()));
                next = std::move(intersection);
            }
            next.insert(n);
            if (next != dom[n]) { dom[n] = std::move(next); changed = true; }
        }
    }
    return dom;
}

inline std::optional<NodeId> immediate_dominator(NodeId n, const DominatorSets& dom) {
    const auto& set = dom.at(n);
    for (auto candidate : set) {
        if (candidate == n) continue;
        bool closest = true;
        for (auto other : set) if (other != n && other != candidate && !dom.at(candidate).count(other)) closest = false;
        if (closest) return candidate;
    }
    return {};
}

} // namespace firestone::ir
