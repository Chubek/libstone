#include <libtrs/GraphCombinators.hpp>
#include <libtrs/StringRewriting.hpp>

int main() {
    firestone::trs::Graph graph;
    auto i = firestone::trs::combinator(graph, "I");
    auto x = graph.add("x");
    auto result = firestone::trs::reduce_combinators(graph, firestone::trs::apply(graph, i, x));
    firestone::trs::StringRewriter strings;
    strings.add_rule({"contract", "xx", "x"});
    return result.root == x && strings.run("xx").value == "x" ? 0 : 1;
}
