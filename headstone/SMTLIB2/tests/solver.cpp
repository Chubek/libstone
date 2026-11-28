#ifdef NDEBUG
#undef NDEBUG
#endif
#include <smtlib2/solver.hpp>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace smtlib2;

namespace {
using Variables = std::vector<std::string>;

void check_model(const SolveResult& result, const Variables& vars, const std::vector<SExpr>& terms) {
    if (result.status != CheckResult::sat) {
        assert(result.model.empty());
        return;
    }
    // Assert exact projection, including unused (and possibly duplicated) names.
    BoolModel projected;
    for (const auto& name : vars) projected.emplace(name, result.model.at(name));
    assert(projected == result.model);
    for (const auto& term : terms) {
        bool value = false;
        assert(BoolSolver::evaluate(term, result.model, value) && value);
    }
}

SolveResult check(const BoolSolver& solver, const Variables& vars, std::string_view source,
                  CheckResult expected) {
    const auto terms = parse(source);
    auto result = solver.check(vars, terms);
    assert(result.status == expected);
    check_model(result, vars, terms);
    return result;
}

void truth_tables() {
    const Variables vars{"p", "q", "r"};
    const BoolSolver solver;
    for (unsigned bits = 0; bits < 8; ++bits) {
        const bool p = bits & 1U, q = bits & 2U, r = bits & 4U;
        const BoolModel model{{"p", p}, {"q", q}, {"r", r}};
        const std::string guards = std::string(p ? "p " : "(not p) ") +
                                   (q ? "q " : "(not q) ") +
                                   (r ? "r " : "(not r) ");
        struct Case { const char* expression; bool value; };
        const Case cases[] = {
            {"true", true},
            {"|false|", false},
            {"|p|", p},
            {"(not p)", !p},
            {"(and p (not q) r)", p && !q && r},
            {"(or (not p) q r)", !p || q || r},
            {"(|xor| p q r)", static_cast<bool>(((bits >> 0) ^ (bits >> 1) ^ (bits >> 2)) & 1U)},
            {"(=> p q r)", !p || !q || r},
            {"(=> p (not q) r (not p))", !p || q || !r},
            {"(= p q r)", p == q && q == r},
            {"(= (not p) q r)", !p == q && q == r},
            {"(distinct p (not q))", p == q},
            {"(distinct p q r)", false},
            {"(ite p (not q) r)", (p && !q) || (!p && r)},
            {"(ite (not p) q (not r))", (!p && q) || (p && !r)},
            {"(xor (not q) (not q))", false},
        };
        for (const auto& entry : cases) {
            bool evaluated = !entry.value;
            assert(BoolSolver::evaluate(parse(entry.expression).front(), model, evaluated));
            assert(evaluated == entry.value);
            check(solver, vars, guards + entry.expression,
                  entry.value ? CheckResult::sat : CheckResult::unsat);
            check(solver, vars, guards + "(not " + entry.expression + ")",
                  entry.value ? CheckResult::unsat : CheckResult::sat);
        }
    }
    check(solver, {"space here", "7", ""}, "|space here| |7| ||", CheckResult::sat);
}

void backtracking_and_projection() {
    const BoolSolver solver;
    check(solver, {}, "", CheckResult::sat);
    const Variables vars{"p", "q", "unused", "p"};
    // With the deterministic false-first branch, p=false forces a conflict
    // on q; propagation on that branch must be undone to discover p=true.
    const auto sat = check(solver, vars, "(or p q) (or p (not q))", CheckResult::sat);
    assert(sat.model.at("p") && !sat.model.at("unused"));
    check(solver, vars, "(or p q) (or p (not q)) (or (not p) q) (or (not p) (not q))",
          CheckResult::unsat);
    check(solver, vars, "(or p p) (not p)", CheckResult::unsat);
    check(solver, vars, "(or p (not p)) (and q q)", CheckResult::sat);
    check(solver, vars, "p p (not p)", CheckResult::unsat);
    check(solver, vars, "true false", CheckResult::unsat);
}

void malformed_and_unsupported() {
    const Variables vars{"x"};
    const BoolSolver solver;
    const ReferenceBoolSolver reference;
    for (const auto* term : {"()", "(x true)", "(not)", "(not x x)", "(and x)",
                             "(or)", "(=> x)", "(= x)", "(distinct x)", "(ite x true)",
                             "4", "\"text\"", "#xA", ":keyword", "missing", "(= 2 3)",
                             "(or true missing)", "(and false missing)",
                             "(ite true true missing)", "(distinct x x missing)"}) {
        bool evaluated = false;
        assert(!BoolSolver::evaluate(parse(term).front(), {{"x", false}}, evaluated));
        // Validation of the whole input precedes an UNSAT verdict, even when
        // a different assertion already entails a contradiction.
        const auto assertions = parse(std::string("false ") + term);
        for (const auto& result : {solver.check(vars, assertions), reference.check(vars, assertions)}) {
            assert(result.status == CheckResult::unknown && result.model.empty());
        }
    }
    auto forged = parse("x (not x)");
    forged[0].children.push_back(forged[1]); // atomic node with children
    forged[1].atom = "not";                // application with atom payload
    for (const auto& term : forged)
        assert(solver.check(vars, {term}).status == CheckResult::unknown);
    check(solver, {"true"}, "", CheckResult::unknown);
    check(solver, {"false"}, "", CheckResult::unknown);
    for (const auto* name : {"not", "and", "or", "xor", "=>", "=", "distinct", "ite"}) {
        check(solver, {name}, "", CheckResult::unknown);
        assert(reference.check({name}, {}).status == CheckResult::unknown);
        bool value;
        assert(!BoolSolver::evaluate(parse(name).front(), {{name, true}}, value));
    }
}

void core_witness_validation() {
    sat_detail::Cnf core({});
    assert(core.declare_symbol("x"));
    const auto terms = parse("x (not x)");
    const auto yes = core.encode_root(terms[0]);
    const auto no = core.encode_root(terms[1]);
    assert(yes && no);
    // The projected witness must satisfy the original term, not just the
    // supplied CNF root, and invalid inputs cannot borrow a root conflict.
    assert(core.solve({yes}, {&terms[1]}).status == CheckResult::unknown);
    assert(core.solve({yes, no}, {&terms[0], nullptr}).status == CheckResult::unknown);
    for (const int invalid : {0, yes + 1, std::numeric_limits<int>::min()})
        assert(core.solve({yes, no, invalid}, {&terms[0], &terms[1], &terms[0]}).status ==
               CheckResult::unknown);
    assert(core.solve({yes, no}, {&terms[0], &terms[1]}).status == CheckResult::unsat);
}

void limits_and_rollback() {
    const auto defaults = BoolSolver::default_max_variables;
    check(BoolSolver(0), {}, "true", CheckResult::sat);
    check(BoolSolver(0), {"x"}, "", CheckResult::unknown);
    check(BoolSolver(1), {"x", "x"}, "", CheckResult::unknown);

    SatLimits bounds;
    bounds.max_cnf_variables = 2;
    check(BoolSolver(defaults, bounds), {"x", "y"}, "x", CheckResult::sat);
    check(BoolSolver(defaults, bounds), {"x", "y"}, "(or x y)", CheckResult::unknown);
    check(BoolSolver(defaults, bounds), {"x", "y", "z"}, "", CheckResult::unknown);

    bounds = {};
    bounds.max_cnf_literals = 1;
    check(BoolSolver(defaults, bounds), {"x"}, "x", CheckResult::sat);
    check(BoolSolver(defaults, bounds), {"x"}, "x x", CheckResult::unknown);
    check(BoolSolver(defaults, bounds), {}, "true", CheckResult::unknown);

    bounds.max_cnf_literals = 2;
    sat_detail::Cnf core(bounds);
    assert(core.declare_symbol("x"));
    const auto base = core.checkpoint();
    const auto terms = parse("(or true x) false true");
    assert(!core.encode_root(terms[0])); // fails after staging the truth literal
    const auto negative = core.encode_root(terms[1]);
    assert(negative && core.solve({negative}, {&terms[1]}).status == CheckResult::unsat);
    core.rollback(base);
    assert(core.declare_symbol("y"));
    const auto positive = core.encode_root(terms[2]);
    assert(positive && core.solve({positive}, {&terms[2]}).model.size() == 2);
    core.rollback(base);
    const auto again = core.encode_root(terms[2]);
    assert(again == positive - 1 && core.solve({again}, {&terms[2]}).model.count("y") == 0);

    bounds = {};
    bounds.max_cnf_variables = 3;
    bounds.max_cnf_literals = 7;
    sat_detail::Cnf partial_gate(bounds);
    assert(partial_gate.declare_symbol("x") && partial_gate.declare_symbol("y"));
    const auto gate_terms = parse("(and x y) true x");
    // The first two binary clauses fit, but the last ternary clause does not.
    // The failed gate must not retain any clauses, variables, or literal cost.
    assert(!partial_gate.encode_root(gate_terms[0]));
    const auto truth = partial_gate.encode_root(gate_terms[1]);
    assert(truth == 3 && partial_gate.solve({truth}, {&gate_terms[1]}).status == CheckResult::sat);
    for (int i = 0; i < 5; ++i) assert(partial_gate.encode_root(gate_terms[2]));
    assert(!partial_gate.encode_root(gate_terms[2]));

    bounds = {};
    bounds.max_work = 0;
    check(BoolSolver(defaults, bounds), {}, "", CheckResult::sat);
    bounds.max_work = 4; // one declaration, one term, one root, one projection
    check(BoolSolver(defaults, bounds), {"x"}, "x", CheckResult::unknown);
    bounds.max_work = 5; // additionally visit the term to validate the model
    const BoolSolver exact(defaults, bounds);
    const auto first = check(exact, {"x"}, "x", CheckResult::sat);
    assert(first.model == check(exact, {"x"}, "x", CheckResult::sat).model);

    bounds.max_work = 7;
    sat_detail::Cnf interrupted(bounds);
    assert(interrupted.declare_symbol("x") && interrupted.declare_symbol("y"));
    const auto work_terms = parse("(and (or x y) (not x)) x");
    // Exhaustion while encoding the final operand must roll back the work
    // and CNF already used to encode the nested gate.
    assert(!interrupted.encode_root(work_terms[0]));
    const auto leaf = interrupted.encode_root(work_terms[1]);
    assert(leaf && interrupted.solve({leaf}, {&work_terms[1]}).status == CheckResult::sat);

    // Five units of construction work, followed by three root inspections.
    bounds.max_work = 7;
    check(BoolSolver(defaults, bounds), {"x"}, "x (not x) x", CheckResult::unknown);
    bounds.max_work = 8;
    check(BoolSolver(defaults, bounds), {"x"}, "x (not x) x", CheckResult::unsat);

    // All eight assignments are forbidden; this proof needs search rather
    // than contradicting root units or propagation at level zero.
    std::string blocked;
    for (unsigned bits = 0; bits < 8; ++bits) {
        blocked += "(or ";
        blocked += bits & 1U ? "(not p) " : "p ";
        blocked += bits & 2U ? "(not q) " : "q ";
        blocked += bits & 4U ? "(not r)) " : "r) ";
    }
    bounds.max_work = 64;
    check(BoolSolver(defaults, bounds), {"p", "q", "r"}, blocked, CheckResult::unknown);
    check(BoolSolver{}, {"p", "q", "r"}, blocked, CheckResult::unsat);

    // A limited run may give up in propagation or while undoing a branch;
    // it must never report a definitive result inconsistent with full search.
    const Variables three{"p", "q", "r"};
    const auto unsatisfiable = parse(blocked);
    const auto satisfiable = parse("(or p q r) (or p (not q)) (or p (not r))");
    for (const auto* formula : {&unsatisfiable, &satisfiable}) {
        const auto expected = formula == &unsatisfiable ? CheckResult::unsat : CheckResult::sat;
        bool determined = false;
        for (std::size_t budget = 0; budget <= 1600; ++budget) {
            bounds.max_work = budget;
            const auto result = BoolSolver(defaults, bounds).check(three, *formula);
            if (result.status == CheckResult::unknown) {
                assert(!determined);
            } else {
                assert(result.status == expected);
                determined = true;
            }
            check_model(result, three, *formula);
        }
        assert(determined);
    }

    bounds = {};
    bounds.max_depth = 0;
    check(BoolSolver(defaults, bounds), {"x"}, "x", CheckResult::sat);
    check(BoolSolver(defaults, bounds), {"x"}, "(not x)", CheckResult::unknown);
    bounds.max_depth = 1;
    check(BoolSolver(defaults, bounds), {"x"}, "(not x)", CheckResult::sat);
    check(BoolSolver(defaults, bounds), {"x"}, "(not (not x))", CheckResult::unknown);
    bounds.max_depth = 10000;
    Limits parsing;
    parsing.max_depth = 513;
    std::string deep;
    for (int i = 0; i < 512; ++i) deep += "(not ";
    deep += "x" + std::string(512, ')');
    check(BoolSolver(defaults, bounds), {"x"}, deep, CheckResult::sat);
    const auto too_deep = parse("(not " + deep + ")", parsing);
    assert(BoolSolver(defaults, bounds).check({"x"}, too_deep).status == CheckResult::unknown);
}

// Use the full specified xorshift sequence, rather than an implementation-
// dependent distribution or random seed, to make failures reproducible.
class Formulas {
public:
    std::uint32_t draw(std::uint32_t n) {
        bits_ ^= bits_ << 13;
        bits_ ^= bits_ >> 17;
        bits_ ^= bits_ << 5;
        return bits_ % n;
    }

    std::string term(unsigned depth) {
        if (!depth || draw(4) == 0) {
            const char* leaves[] = {"a", "b", "c", "d", "|a|", "|a b|", "false", "true"};
            return leaves[draw(8)];
        }
        const char* operations[] = {"not", "and", "or", "xor", "=>", "=", "distinct", "ite"};
        const auto op = draw(8);
        const auto argc = op == 0 ? 1U : op == 7 ? 3U : 2 + draw(4);
        auto result = std::string("(") + operations[op];
        for (unsigned i = 0; i < argc; ++i) result += " " + term(depth - 1);
        return result + ')';
    }

private:
    std::uint32_t bits_ = 0x87cf843bU;
};

void differential() {
    const Variables vars{"d", "a", "c", "a b", "b", "unused"};
    const BoolSolver solver;
    const ReferenceBoolSolver reference;
    Formulas formulas;
    unsigned sats = 0, unsats = 0;
    for (unsigned test = 0; test < 640; ++test) {
        std::string input;
        const auto length = 1 + formulas.draw(6);
        for (unsigned i = 0; i < length; ++i)
            input += formulas.term(1 + formulas.draw(4)) + ' ';
        const auto assertions = parse(input);
        const auto expected = reference.check(vars, assertions);
        assert(expected.status != CheckResult::unknown);
        const auto actual = solver.check(vars, assertions);
        assert(actual.status == expected.status);
        sats += actual.status == CheckResult::sat;
        unsats += actual.status == CheckResult::unsat;
        check_model(actual, vars, assertions);
        const auto repeated = solver.check(vars, assertions);
        assert(repeated.status == actual.status && repeated.model == actual.model);
    }
    assert(sats > 100 && unsats > 100);
}
}

int main() {
    truth_tables();
    backtracking_and_projection();
    malformed_and_unsupported();
    core_witness_validation();
    limits_and_rollback();
    differential();
}
