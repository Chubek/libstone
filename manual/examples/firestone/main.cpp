#include <libeqsat/Saturation.hpp>

#include <iostream>

int main() {
    using firestone::rewrite::parse;
    firestone::eqsat::EGraph graph;
    const auto root = graph.add(parse("(add (mul x 1) 0)"));
    firestone::eqsat::Saturator runner;
    runner.add_rule({"mul-one", parse("(mul ?x 1)"), parse("?x")});
    runner.add_rule({"add-zero", parse("(add ?x 0)"), parse("?x")});
    runner.run(graph);
    const auto best = graph.extract(root);
    std::cout << firestone::rewrite::to_string(best.term) << '\n';
    return firestone::rewrite::to_string(best.term) == "x" ? 0 : 1;
}
