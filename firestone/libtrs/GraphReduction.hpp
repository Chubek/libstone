#pragma once

#include "GraphRewriting.hpp"

namespace firestone::trs {

// A reduction retains the graph arena and returns a new root. This makes it
// possible to inspect both the original and reduced expressions, including
// their shared subgraphs.
class GraphReducer {
public:
    void add_rule(GraphRule rule) { rewriter_.add_rule(std::move(rule)); }
    const std::vector<GraphRule>& rules() const { return rewriter_.rules(); }
    GraphRewriteResult run(Graph& graph, GraphId root, std::size_t iteration_limit = 100,
                           std::size_t application_limit = 10000,
                           RewriteOrder order = RewriteOrder::BottomUp) const {
        return rewriter_.run(graph, root, iteration_limit, application_limit, order);
    }
private:
    GraphRewriter rewriter_;
};

} // namespace firestone::trs
