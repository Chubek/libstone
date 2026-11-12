#include <cassert>
#include <supersonic/Supersonic.hpp>
using namespace moonstone::supersonic;
int main() {
    Problem p;
    p.grammar.variables = {"x", "y"};
    p.grammar.constants = {0, 1};
    p.examples = {
        {{{"x", 2}, {"y", 3}}, 5}, {{{"x", 9}, {"y", 4}}, 13}, {{{"x", 10}, {"y", 10}}, 20}};
    auto r = solve(p);
    assert(r.status == SolveStatus::Found && r.program.has_value());
    for (auto &ex : p.examples)
        assert(evaluate(*r.program, ex.inputs) == ex.expected);
    p.budget.max_candidates = 1;
    auto s = solve(p);
    assert(s.status == SolveStatus::BudgetExhausted && s.candidates == 1);
    p.budget.max_candidates = 100000;
    for (auto op : {Op::Constant, Op::Variable, static_cast<Op>(999)}) {
        auto invalid = p;
        invalid.grammar.operations.push_back(op);
        assert(solve(invalid).status == SolveStatus::InvalidInput);
    }
    for (auto variable : {"", "1", "(x+y)", "x-y"}) {
        auto invalid = p;
        invalid.grammar.variables = {variable};
        assert(solve(invalid).status == SolveStatus::InvalidInput);
    }
    auto duplicate = p;
    duplicate.grammar.variables = {"x", "x"};
    assert(solve(duplicate).status == SolveStatus::InvalidInput);
    auto missing = p;
    missing.examples[0].inputs.erase("x");
    assert(solve(missing).status == SolveStatus::InvalidInput);
    assert(!evaluate(Expr{Op::Constant, 1, {}, {{}}}, {}));
    assert(!evaluate(Expr{Op::Variable, 0, "x", {{}}}, {{"x", 1}}));
    assert(!evaluate(Expr{static_cast<Op>(999), 0, {}, {}}, {}));
    assert(!evaluate(Expr{Op::Add, 0, {}, {{}}}, {}));
    Expr term{Op::Neg, 0, {}, {{Op::Constant, 1, {}, {}}}};
    assert(!evaluate(term, {}, 1));
    assert(!evaluate(term, {}, 256, 1));
    assert(evaluate(term, {}) == -1);
    Problem exact;
    exact.grammar.constants = {0};
    exact.grammar.operations = {};
    exact.examples = {{{}, 1}};
    exact.budget.max_candidates = 1;
    assert(solve(exact).status ==
           SolveStatus::UnsatisfiableWithinBound); // Exhausted the grammar, not the budget.
    exact.examples[0].expected = 0;
    assert(solve(exact).status == SolveStatus::Found);
    Problem syntax;
    syntax.grammar.variables = {"x", "y"};
    syntax.grammar.operations = {Op::Add};
    syntax.examples = {{{{"x", 1}, {"y", 2}}, 3}};
    syntax.budget.max_cost = 3;
    syntax.predicate = [](const Expr &e) { return e.print() == "(y+x)"; };
    assert(solve(syntax).status == SolveStatus::Found);
}
