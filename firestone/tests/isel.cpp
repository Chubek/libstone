#include "Test.hpp"
#include "../libisel/BURS.hpp"
#include "../libisel/DAGDynProg.hpp"
#include "../libisel/DynamicProg.hpp"
#include "../libisel/GraphCovering.hpp"
#include "../libisel/ILP.hpp"
#include "../libisel/MacroExpansion.hpp"
#include "../libisel/MaximalMunch.hpp"
#include "../libisel/PeepholeOpt.hpp"
#include "../libisel/TreeDecomposition.hpp"
#include "../libisel/UniversalSMT.hpp"
using namespace firestone::isel;
using firestone::rewrite::parse;
static Rule rule(const std::string& name, const std::string& pattern, double cost) {
    Rule r; r.name = name; r.pattern = parse(pattern); r.instruction = name; r.cost = cost; return r;
}
int main() { return test_main([] {
    Rules rules;
    rules.add(rule("X", "x", 1)); rules.add(rule("Y", "y", 1));
    rules.add(rule("ADD", "(add ?a ?b)", 2));
    rules.add(rule("MUL", "(mul ?a ?b)", 3));
    rules.add(rule("FMA", "(add (mul ?a ?b) ?c)", 2));
    Graph g; auto root = g.import(parse("(add (mul x y) x)"));
    auto dp = DynamicProg{}.select(g, rules, {root});
    CHECK(dp.stop == Stop::Complete); CHECK(dp.cost == 5); CHECK(dp.optimal);
    CHECK(dp.instructions.back().opcode == "FMA");
    CHECK(BURS{}.select(g, rules, {root}).cost == dp.cost);
    CHECK(MaximalMunch{}.select(g, rules, {root}).cost == dp.cost);
    CHECK(DAGDynProg{}.select(g, rules, {root}).cost == dp.cost);
    CHECK(GraphCovering{}.select(g, rules, {root}).cost == dp.cost);
    CHECK(ILP{}.select(g, rules, {root}).optimal);
    CHECK(TreeDecomposition{}.select(g, rules, {root}).cost == dp.cost);
    CHECK(UniversalSMT{}.select(g, rules, {root}).cost == dp.cost);
    CHECK(GraphCovering{}.select(g, rules, {root}, 0).stop == Stop::SearchLimit);
    CHECK(TreeDecomposition{}.select(g, rules, {root}, 0).stop == Stop::SearchLimit);
    Graph shared; auto x = shared.add("x"), y = shared.add("y");
    auto mul = shared.add("mul", {x, y}); auto add = shared.add("add", {mul, x});
    auto sum = shared.add("add", {add, mul});
    auto selected = GraphCovering{}.select(shared, rules, {sum});
    CHECK(selected.cost == 9); CHECK(selected.instructions.size() == 5);
    CHECK(!DAGDynProg{}.select(shared, rules, {sum}).optimal);
    CHECK(TreeDecomposition{}.select(shared, rules, {sum}).cost == selected.cost);
    CHECK(UniversalSMT{}.select(shared, rules, {sum}).cost == selected.cost);
    // A root may not disappear inside another tile.
    CHECK(GraphCovering{}.select(shared, rules, {add, mul}).instructions.size() == 4);
    Problem problem(g, rules, {root});
    std::vector<std::vector<std::int64_t>> forbid_fma;
    for (std::size_t i = 0; i < problem.tiles.size(); ++i)
        if (rules[problem.tiles[i].rule].instruction == "FMA") forbid_fma.push_back({-static_cast<std::int64_t>(i + 1)});
    CHECK(UniversalSMT{}.select(problem, forbid_fma).cost == 8);
    CHECK(UniversalSMT{}.select(problem, {{}}).stop == Stop::Infeasible);
    throws<std::invalid_argument>([&] { UniversalSMT{}.select(problem, {{0}}); });
    Rules none;
    CHECK(DynamicProg{}.select(g, none, {root}).stop == Stop::Infeasible);
    CHECK(GraphCovering{}.select(g, none, {root}).stop == Stop::Infeasible);
    CHECK(TreeDecomposition{}.select(g, none, {root}).stop == Stop::Infeasible);
    CHECK(DynamicProg{}.select(g, rules, {}).instructions.empty());
    throws<std::out_of_range>([&] { g.add("bad", {g.size()}); });
    throws<std::invalid_argument>([&] { rules.add(rule("bad", "?x", 1)); });
    throws<std::invalid_argument>([&] { rules.add(rule("bad", "x", -1)); });
    auto guarded = rule("guard", "x", 0); guarded.guard = [](const Graph&, NodeId, const Bindings&) { return false; };
    rules.add(guarded); CHECK(DynamicProg{}.select(g, rules, {root}).cost == 5);
    // Different node IDs are not a repeated-variable match, even for equal leaves.
    Rules repeated; repeated.add(rule("X", "x", 1)); repeated.add(rule("same", "(add ?x ?x)", 1));
    Graph distinct; auto dr = distinct.import(parse("(add x x)"));
    CHECK(DynamicProg{}.select(distinct, repeated, {dr}).stop == Stop::Infeasible);
    Graph identical; auto ix = identical.add("x"); auto ir = identical.add("add", {ix, ix});
    CHECK(DAGDynProg{}.select(identical, repeated, {ir}).cost == 2);
    firestone::ir::DataflowGraph dataflow;
    auto a = dataflow.constant(7); auto b = dataflow.constant(9);
    auto c = dataflow.operation(firestone::ir::Opcode::Add, firestone::ir::Type::integer(), {a, b});
    auto adapted = from_dataflow(dataflow);
    CHECK(adapted.node(a).source->operands[0].integer == 7);
    CHECK(adapted.node(c).inputs == std::vector<NodeId>({a, b}));
    firestone::eqsat::EGraph eg; auto er = eg.add(parse("(add x x)"));
    auto extraction = firestone::eqsat::extract_dag(eg, er); auto imported = from_extraction(extraction);
    CHECK(imported.node(extraction.root).inputs[0] == imported.node(extraction.root).inputs[1]);
    // Compare independent enumeration of all tile subsets with every exact solver.
    for (unsigned variant = 0; variant < 8; ++variant) {
        Rules choices;
        choices.add(rule("X", "x", 1)); choices.add(rule("Y", "y", 2));
        choices.add(rule("ADD", "(add ?a ?b)", 1 + (variant & 1)));
        choices.add(rule("MUL", "(mul ?a ?b)", 2));
        choices.add(rule("FMA", "(add (mul ?a ?b) ?c)", 1 + variant));
        choices.add(rule("ALL", "(add (mul x y) x)", 2 + (variant & 3)));
        Problem instance(g, choices, {root}); double optimum = 1e9;
        CHECK(instance.tiles.size() < 20);
        for (std::size_t mask = 0; mask < (std::size_t{1} << instance.tiles.size()); ++mask) {
            std::map<NodeId, unsigned> covered; double cost = 0;
            for (std::size_t i = 0; i < instance.tiles.size(); ++i) if (mask & (std::size_t{1} << i)) {
                cost += choices[instance.tiles[i].rule].cost;
                for (auto n : instance.tiles[i].covered) ++covered[n];
            }
            bool legal = true;
            for (auto n : instance.live) if (covered[n] != 1) legal = false;
            if (legal) optimum = std::min(optimum, cost);
        }
        CHECK(GraphCovering{}.select(g, choices, {root}).cost == optimum);
        CHECK(ILP{}.select(g, choices, {root}).cost == optimum);
        CHECK(UniversalSMT{}.select(g, choices, {root}).cost == optimum);
        CHECK(TreeDecomposition{}.select(g, choices, {root}).cost == optimum);
        CHECK(BURS{}.select(g, choices, {root}).cost == optimum);
    }
    MacroExpansion macros; macros.add_rule({"lower", parse("(twice ?x)"), parse("(add ?x ?x)")});
    CHECK(macros.expand(parse("(twice x)")).term == parse("(add x x)"));
    PeepholeOpt peepholes;
    peepholes.add_rule({{parse("(mov ?a ?b)"), parse("(mov ?b ?a)")}, {parse("(mov ?a ?b)")}, {}});
    auto optimized = peepholes.run({parse("(mov r1 r2)"), parse("(mov r2 r1)")});
    CHECK(optimized.code.size() == 1); CHECK(optimized.applications == 1);
    CHECK(peepholes.run({parse("(mov r1 r2)"), parse("(mov r3 r1)")}).applications == 0);
    PeepholeOpt cycling; cycling.add_rule({{parse("a")}, {parse("b")}, {}}); cycling.add_rule({{parse("b")}, {parse("a")}, {}});
    CHECK(cycling.run({parse("a")}, 3).limited);
    throws<std::invalid_argument>([&] { peepholes.add_rule({{parse("a")}, {parse("?x")}, {}}); });
}); }
