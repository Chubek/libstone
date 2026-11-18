#pragma once
#include "EGraph.hpp"

namespace firestone::eqsat {

struct ExtractedNode { std::string op; std::vector<std::size_t> children; };
struct DAGExtraction {
    std::vector<ExtractedNode> nodes; // Topologically ordered, children precede parents.
    std::size_t root = 0;
    double tree_cost = 0;
};

// Sharing is recovered after minimum-tree-cost extraction; this is not a
// minimum-DAG-cost solver (a different global optimization problem).
inline DAGExtraction extract_dag(EGraph& graph, EClassId root, std::function<double(const ENode&)> local_cost = {}) {
    auto extracted = graph.extract(root, std::move(local_cost));
    DAGExtraction result; result.tree_cost = extracted.cost;
    std::map<rewrite::Term, std::size_t> memo;
    std::function<std::size_t(const rewrite::Term&)> build = [&](const rewrite::Term& term) {
        auto found = memo.find(term);
        if (found != memo.end()) return found->second;
        std::vector<std::size_t> children;
        for (const auto& child : term.children) children.push_back(build(child));
        auto id = result.nodes.size(); result.nodes.push_back({term.op, std::move(children)}); memo.emplace(term, id); return id;
    };
    result.root = build(extracted.term); return result;
}

// Budget counts expanded tree nodes (shared children count once per occurrence).
inline rewrite::Term expand(const DAGExtraction& extraction,
                           std::size_t node_limit = std::numeric_limits<std::size_t>::max()) {
    if (extraction.root >= extraction.nodes.size()) throw std::invalid_argument("invalid extraction root");
    for (std::size_t id = 0; id < extraction.nodes.size(); ++id) {
        const auto& node = extraction.nodes[id];
        if (node.op.empty()) throw std::invalid_argument("empty extraction operator");
        for (auto child : node.children) if (child >= id)
            throw std::invalid_argument("extraction nodes must be topologically ordered");
    }
    // Count occurrences by propagating the root multiplicity down the DAG.
    // Unreachable nodes are validated above but not counted or materialized.
    std::vector<std::size_t> occurrences(extraction.root + 1);
    occurrences.back() = 1;
    auto remaining = node_limit;
    for (std::size_t next = occurrences.size(); next != 0;) {
        auto id = --next;
        auto count = occurrences[id];
        if (count > remaining) throw std::length_error("extraction expansion exceeds node limit");
        remaining -= count;
        if (!count) continue;
        for (auto child : extraction.nodes[id].children) {
            if (count > remaining || occurrences[child] > remaining - count)
                throw std::length_error("extraction expansion exceeds node limit");
            occurrences[child] += count;
        }
    }
    auto build = [&](const auto& self, std::size_t id) -> rewrite::Term {
        const auto& node = extraction.nodes[id];
        std::vector<rewrite::Term> children;
        children.reserve(node.children.size());
        for (auto child : node.children) children.push_back(self(self, child));
        return {node.op, std::move(children)};
    };
    return build(build, extraction.root);
}

} // namespace firestone::eqsat
