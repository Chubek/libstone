#pragma once

#include "TermRewriting.hpp"
#include <map>
#include <functional>

namespace firestone::trs {

using GraphId = std::size_t;

// An append-only, hash-consed acyclic term graph. IDs remain valid after a
// rewrite; old roots still refer to the original expression.
class Graph {
public:
    struct Node {
        std::string op;
        std::vector<GraphId> children;
    };
    GraphId add(std::string op, std::vector<GraphId> children = {}) {
        if (op.empty()) throw std::invalid_argument("graph operator is empty");
        for (auto child : children) if (child >= nodes_.size()) throw std::out_of_range("invalid graph child");
        auto key = std::make_pair(op, children);
        auto it = index_.find(key);
        if (it != index_.end()) return it->second;
        auto id = nodes_.size();
        nodes_.push_back({std::move(op), std::move(children)});
        index_.emplace(std::move(key), id);
        return id;
    }
    GraphId from_term(const Term& term) {
        validate_term(term);
        std::vector<GraphId> children;
        for (const auto& child : term.children) children.push_back(from_term(child));
        return add(term.op, std::move(children));
    }
    Term to_term(GraphId root) const {
        const auto& n = node(root);
        std::vector<Term> children;
        for (auto child : n.children) children.push_back(to_term(child));
        return {n.op, std::move(children)};
    }
    const Node& node(GraphId id) const {
        if (id >= nodes_.size()) throw std::out_of_range("invalid graph node");
        return nodes_[id];
    }
    std::size_t size() const { return nodes_.size(); }
private:
    std::vector<Node> nodes_;
    std::map<std::pair<std::string, std::vector<GraphId>>, GraphId> index_;
};

using GraphBindings = std::map<std::string, GraphId>;

inline bool graph_match(const Graph& graph, const Pattern& pattern, GraphId root, GraphBindings& bindings) {
    validate_term(pattern, true);
    GraphBindings tentative = bindings;
    std::function<bool(const Pattern&, GraphId)> walk = [&](const Pattern& p, GraphId id) {
        const auto& n = graph.node(id);
        if (is_variable(p)) {
            auto it = tentative.find(p.op);
            if (it != tentative.end()) return it->second == id;
            tentative.emplace(p.op, id);
            return true;
        }
        if (p.op != n.op || p.children.size() != n.children.size()) return false;
        for (std::size_t i = 0; i < n.children.size(); ++i)
            if (!walk(p.children[i], n.children[i])) return false;
        return true;
    };
    if (!walk(pattern, root)) return false;
    bindings = std::move(tentative);
    return true;
}

inline GraphId graph_substitute(Graph& graph, const Pattern& pattern, const GraphBindings& bindings) {
    validate_term(pattern, true);
    if (is_variable(pattern)) {
        auto it = bindings.find(pattern.op);
        if (it == bindings.end()) throw std::invalid_argument("unbound graph variable: " + pattern.op);
        graph.node(it->second);
        return it->second;
    }
    std::vector<GraphId> children;
    for (const auto& child : pattern.children) children.push_back(graph_substitute(graph, child, bindings));
    return graph.add(pattern.op, std::move(children));
}

struct GraphRule {
    std::string name;
    Pattern lhs, rhs;
    std::function<bool(const Graph&, const GraphBindings&)> guard;
    GraphRule(std::string name, Pattern lhs, Pattern rhs,
              std::function<bool(const Graph&, const GraphBindings&)> guard = {})
        : name(std::move(name)), lhs(std::move(lhs)), rhs(std::move(rhs)), guard(std::move(guard)) {
        validate_rule(this->lhs, this->rhs);
    }
};

struct GraphRewriteResult {
    GraphId root;
    RewriteStop stop = RewriteStop::NormalForm;
    std::size_t iterations = 0, applications = 0;
    std::vector<std::string> applied_rules;
};

class GraphRewriter {
public:
    void add_rule(GraphRule rule) {
        validate_rule(rule.lhs, rule.rhs);
        rules_.push_back(std::move(rule));
    }
    const std::vector<GraphRule>& rules() const { return rules_; }
    GraphRewriteResult run(Graph& graph, GraphId root, std::size_t iteration_limit = 100,
                           std::size_t application_limit = 10000,
                           RewriteOrder order = RewriteOrder::BottomUp) const {
        graph.node(root);
        GraphRewriteResult result;
        result.root = root;
        bool limited = false;
        auto apply = [&](GraphId id) {
            for (const auto& rule : rules_) {
                GraphBindings bindings;
                if (!graph_match(graph, rule.lhs, id, bindings) || (rule.guard && !rule.guard(graph, bindings))) continue;
                // Test the replacement against the current node before allocating
                // anything, especially when the application budget is exhausted.
                auto identity = bindings;
                if (graph_match(graph, rule.rhs, id, identity)) continue;
                if (result.applications == application_limit) { limited = true; return id; }
                const auto replacement = graph_substitute(graph, rule.rhs, bindings);
                ++result.applications;
                result.applied_rules.push_back(rule.name);
                return replacement;
            }
            return id;
        };
        for (std::size_t pass = 0; pass < iteration_limit; ++pass) {
            std::map<GraphId, GraphId> memo;
            std::function<GraphId(GraphId)> visit = [&](GraphId id) -> GraphId {
                auto found = memo.find(id);
                if (found != memo.end()) return found->second;
                auto current = order == RewriteOrder::TopDown ? apply(id) : id;
                // Copy the node: add() may grow the graph and invalidate references.
                auto n = graph.node(current);
                for (auto& child : n.children) {
                    if (limited) break;
                    child = visit(child);
                }
                current = graph.add(n.op, std::move(n.children));
                if (order == RewriteOrder::BottomUp && !limited) current = apply(current);
                memo.emplace(id, current);
                return current;
            };
            const auto previous = result.root;
            const auto next = visit(previous);
            result.root = next;
            ++result.iterations;
            if (limited) { result.stop = RewriteStop::ApplicationLimit; return result; }
            if (next == previous) return result;
            // IDs are canonical, so identical roots indicate a fixed point.
        }
        result.stop = RewriteStop::IterationLimit;
        return result;
    }
private:
    std::vector<GraphRule> rules_;
};

} // namespace firestone::trs
