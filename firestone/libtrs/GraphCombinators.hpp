#pragma once

#include "GraphReduction.hpp"

namespace firestone::trs {

// Untyped SKI calculus. Application is the binary 'app' node, and I, K, S
// are nullary nodes. The S rule inserts both occurrences of z using the
// same graph ID, preserving sharing rather than duplicating its subgraph.
inline GraphId combinator(Graph& graph, const std::string& symbol) {
    if (symbol != "I" && symbol != "K" && symbol != "S")
        throw std::invalid_argument("unknown combinator: " + symbol);
    return graph.add(symbol);
}

inline GraphId apply(Graph& graph, GraphId function, GraphId argument) {
    return graph.add("app", {function, argument});
}

inline GraphReducer combinator_reducer() {
    GraphReducer reducer;
    reducer.add_rule({"I", parse("(app I ?x)"), parse("?x")});
    reducer.add_rule({"K", parse("(app (app K ?x) ?y)"), parse("?x")});
    reducer.add_rule({"S", parse("(app (app (app S ?x) ?y) ?z)"),
                      parse("(app (app ?x ?z) (app ?y ?z))")});
    return reducer;
}

inline GraphRewriteResult reduce_combinators(Graph& graph, GraphId root,
                                              std::size_t iteration_limit = 100,
                                              std::size_t application_limit = 10000) {
    return combinator_reducer().run(graph, root, iteration_limit, application_limit);
}

} // namespace firestone::trs
