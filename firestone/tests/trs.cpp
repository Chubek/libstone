#include "Test.hpp"
#include "../libtrs/GraphCombinators.hpp"
#include "../libtrs/StringRewriting.hpp"

using namespace firestone::trs;

int main() { return test_main([] {
    Rewriter terms;
    terms.add_rule({"identity", parse("(id ?x)"), parse("?x")});
    CHECK(terms.run(parse("(id (id a))")).term == Term("a"));
    CHECK(parse(to_string(parse("(f ?x)"))) == parse("(f ?x)"));

    StringRewriter strings;
    throws<std::invalid_argument>([] { StringRule("bad", "", "a"); });
    strings.add_rule({"double", "aa", "a"});
    strings.add_rule({"delete", "b", ""});
    auto text = strings.run("baaa");
    CHECK(text.value == "a");
    CHECK(text.applications == 3);
    CHECK(text.stop == RewriteStop::NormalForm);
    CHECK(strings.run("aaaa", 100, 0).stop == RewriteStop::ApplicationLimit);
    CHECK(strings.run("aaaa", 0).stop == RewriteStop::IterationLimit);
    StringRewriter cycling;
    cycling.add_rule({"a", "a", "b"});
    cycling.add_rule({"b", "b", "a"});
    CHECK(cycling.run("a", 100, 3).stop == RewriteStop::ApplicationLimit);
    CHECK(cycling.run("a", 3).stop == RewriteStop::IterationLimit);
    StringRewriter noop;
    noop.add_rule({"same", "x", "x"});
    CHECK(noop.run("x").applications == 0);
    CHECK(noop.run("x", 0).stop == RewriteStop::IterationLimit);

    Graph graph;
    auto original = graph.from_term(parse("(pair (id x) (id x))"));
    CHECK(graph.node(original).children[0] == graph.node(original).children[1]);
    CHECK(graph.from_term(graph.to_term(original)) == original);
    throws<std::out_of_range>([&] { graph.add("bad", {graph.size()}); });
    throws<std::invalid_argument>([&] { graph.add(""); });
    throws<std::out_of_range>([&] { graph.node(graph.size()); });
    GraphBindings bindings;
    CHECK(graph_match(graph, parse("(pair ?x ?x)"), original, bindings));
    CHECK(bindings.at("?x") == graph.node(original).children[0]);
    CHECK(!graph_match(graph, parse("(pair ?x x)"), original, bindings));
    CHECK(bindings.size() == 1);
    throws<std::invalid_argument>([&] { graph_substitute(graph, parse("?missing"), bindings); });
    throws<std::invalid_argument>([] { GraphRule("bad", parse("a"), parse("?x")); });

    GraphRewriter rewriter;
    rewriter.add_rule({"id", parse("(id ?x)"), parse("?x")});
    auto rewritten = rewriter.run(graph, original);
    CHECK(graph.to_term(rewritten.root) == parse("(pair x x)"));
    CHECK(graph.to_term(original) == parse("(pair (id x) (id x))"));
    CHECK(rewritten.applications == 1); // shared redex visited once
    CHECK(rewritten.stop == RewriteStop::NormalForm);
    auto before_limit = graph.size();
    CHECK(rewriter.run(graph, original, 100, 0).stop == RewriteStop::ApplicationLimit);
    CHECK(graph.size() == before_limit);
    CHECK(rewriter.run(graph, original, 0).stop == RewriteStop::IterationLimit);
    GraphRewriter identity;
    identity.add_rule({"same", parse("(id ?x)"), parse("(id ?x)")});
    CHECK(identity.run(graph, original, 100, 0).stop == RewriteStop::NormalForm);
    // A spent application budget must not mask an already-normal graph.
    GraphRewriter bounded;
    bounded.add_rule({"id", parse("(id ?x)"), parse("?x")});
    auto one_application = bounded.run(graph, original, 100, 1);
    CHECK(one_application.stop == RewriteStop::NormalForm);
    CHECK(graph.to_term(one_application.root) == parse("(pair x x)"));
    CHECK(one_application.applications == 1);
    CHECK(rewriter.run(graph, original, 100, 100, RewriteOrder::TopDown).root == rewritten.root);
    GraphRewriter guarded;
    guarded.add_rule({"conditional", parse("(id ?x)"), parse("?x"),
                      [](const Graph& g, const GraphBindings& b) { return g.node(b.at("?x")).op == "safe"; }});
    CHECK(guarded.run(graph, original).root == original);
    GraphRewriter oscillating;
    oscillating.add_rule({"a-b", parse("a"), parse("b")});
    oscillating.add_rule({"b-a", parse("b"), parse("a")});
    CHECK(oscillating.run(graph, graph.add("a"), 3).stop == RewriteStop::IterationLimit);

    Graph combinators;
    auto i = combinator(combinators, "I");
    auto k = combinator(combinators, "K");
    auto s = combinator(combinators, "S");
    auto x = combinators.add("x");
    auto y = combinators.add("y");
    CHECK(reduce_combinators(combinators, apply(combinators, i, x)).root == x);
    CHECK(reduce_combinators(combinators, apply(combinators, apply(combinators, k, x), y)).root == x);
    auto skk = apply(combinators, apply(combinators, apply(combinators, s, k), k), x);
    auto reduced = reduce_combinators(combinators, skk);
    CHECK(reduced.root == x);
    CHECK(reduced.stop == RewriteStop::NormalForm);
    throws<std::invalid_argument>([&] { combinator(combinators, "Z"); });
    auto z = combinators.from_term(parse("(long (tail a))"));
    auto shared = apply(combinators, apply(combinators, apply(combinators, s, i), i), z);
    auto one_step = reduce_combinators(combinators, shared, 1);
    CHECK(one_step.stop == RewriteStop::IterationLimit);
    // The result of the S expansion has the same z node on both branches.
    const auto& branches = combinators.node(one_step.root).children;
    CHECK(branches.size() == 2);
    CHECK(branches[0] == branches[1]);
    CHECK(combinators.node(branches[0]).children[1] == z);
}); }
