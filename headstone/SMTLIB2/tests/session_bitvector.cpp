#ifdef NDEBUG
#undef NDEBUG
#endif
#include <smtlib2/bv_solver.hpp>
#include <smtlib2/session.hpp>
#include <cassert>

using namespace smtlib2;

namespace {
void exhaustive_small_values() {
    bv_solver::Environment env{{"x", 3}, {"y", 3}, {"unused", 64}};
    for (const std::string op : {"bvadd", "bvsub", "bvmul", "bvand", "bvor", "bvxor", "bvult"}) {
        const auto term = parse("(" + op + " x y)").at(0);
        const auto square = parse("(= (" + op + " x x) y)");
        if (op != "bvult") {
            auto answer = bv_solver::check(env, square);
            assert(answer.status == CheckResult::sat && answer.model.size() == 3);
            assert(bv_solver::value(square[0], env, answer.model));
        }
        for (unsigned x = 0; x < 8; ++x) for (unsigned y = 0; y < 8; ++y) {
            const unsigned expected = op == "bvadd" ? x+y : op == "bvsub" ? x+8-y :
                op == "bvmul" ? x*y : op == "bvand" ? x&y : op == "bvor" ? x|y :
                op == "bvxor" ? x^y : x<y;
            assert(bv_solver::value(term, env, {{"x", x}, {"y", y}}) == (expected & 7));
        }
    }
    assert(bv_solver::check(env, parse("(= (bvmul x x) #b011)")).status == CheckResult::unsat);
    auto limited = bv_solver::check(env, parse("(= x y)"), 5);
    assert(limited.status == CheckResult::unknown && limited.model.empty());
    assert(bv_solver::check({}, parse("(= #b0 #b0)"), 16, 6).status == CheckResult::unknown);
    auto overflow = parse("(= (bvadd #xffffffffffffffff #x0000000000000001) #x0000000000000000)");
    assert(bv_solver::check({}, overflow).status == CheckResult::sat);
}

void search_and_budget() {
    const bv_solver::Environment env{{"x", 3}, {"y", 3}, {"unused", 64}};
    for (unsigned k = 0; k < 8; ++k) {
        const auto assertions = parse("(= (bvadd x y) #b000) (bvult y x) (= (bvmul x y) " +
                                      bv_solver::literal(3, k) + ")");
        const auto holds = [k](std::uint64_t x, std::uint64_t y) {
            return (x + y) % 8 == 0 && y < x && x * y % 8 == k;
        };
        bool found = false;
        for (unsigned x = 0; x < 8; ++x) for (unsigned y = 0; y < 8; ++y)
            found |= holds(x, y);
        const auto result = bv_solver::check(env, assertions);
        assert(result.status == (found ? CheckResult::sat : CheckResult::unsat));
        if (found) {
            assert(result.model.size() == 3 && result.model.at("unused") == 0);
            assert(holds(result.model.at("x"), result.model.at("y")));
            assert(result.model == bv_solver::check(env, assertions).model);
        } else assert(result.model.empty());
    }

    const bv_solver::Environment flag{{"flag", 0}};
    auto exact = [&](std::string_view assertion, std::size_t first, CheckResult status) {
        const auto terms = parse(assertion);
        for (std::size_t budget = 0; budget < first; ++budget) {
            const auto result = bv_solver::check(flag, terms, 16, budget);
            assert(result.status == CheckResult::unknown && result.model.empty());
            assert(result.reason == "BV work limit");
        }
        assert(bv_solver::check(flag, terms, 16, first).status == status);
    };
    exact("flag", 7, CheckResult::sat); // two assignments, plus final witness check
    exact("(distinct flag flag)", 12, CheckResult::unsat);

    const auto last = bv_solver::check({{"x", 16}}, parse("(= x #xffff)"));
    assert(last.status == CheckResult::sat && last.model.at("x") == 65535);
    assert(bv_solver::check({{"x", 17}}, parse("(= x x)"), 100).status == CheckResult::unknown);
}

void width_and_input_limits() {
    for (const auto* t : {
        "(= (bvneg #b1) #b1)",
        "(= (bvnot #b0) #b1)",
        "(= (bvmul #xffffffffffffffff #xffffffffffffffff) #x0000000000000001)",
        "(= (bvsub #x0000000000000000 #x0000000000000001) #xffffffffffffffff)",
        "(bvult #x7fffffffffffffff #x8000000000000000)"
    }) assert(bv_solver::check({}, parse(t)).status == CheckResult::sat);
    assert(bv_solver::literal(64, UINT64_MAX) == "#b" + std::string(64, '1'));
    assert(bv_solver::value(parse(bv_solver::literal(64, UINT64_MAX)).front(), {}, {}) == UINT64_MAX);

    const bv_solver::Environment env{{"flag", 0}, {"x", 1}};
    const auto both = parse("(=> (bvult #b0 x) flag)").front();
    for (const auto& invalid : {bv_solver::Model{{"flag", 0}, {"x", 2}},
                                bv_solver::Model{{"flag", 2}, {"x", 0}}}) {
        bool rejected = false;
        try { (void)bv_solver::value(both, env, invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    bool missing = false;
    try { (void)bv_solver::value(both, env, {{"x", 0}}); }
    catch (const std::out_of_range&) { missing = true; }
    assert(missing);

    auto reject = [](const SExpr& term) {
        bool rejected = false;
        try { (void)bv_solver::check({}, {parse("false").front(), term}); }
        catch (const Error&) { rejected = true; }
        assert(rejected);
    };
    reject(parse("(= #b0 #b00)").front());
    reject(parse("(= #b" + std::string(65, '1') + " #b0)").front());
    reject(parse("(or true (bvunsupported #b0))").front());
    auto forged_atom = parse("#b0").front();
    forged_atom.children.push_back(parse("missing").front());
    auto equality = parse("(= #b0 #b0)").front();
    equality.children[1] = std::move(forged_atom);
    reject(equality);
    equality = parse("(= #b0 #b0)").front();
    equality.atom = "ignored payload";
    reject(equality);

    auto at_cap = std::string("true");
    for (unsigned i = 0; i < 15; ++i) at_cap = "(and " + at_cap + " " + at_cap + ")";
    at_cap = "(not " + at_cap + ")"; // 65,536 instructions
    assert(bv_solver::check({}, parse(at_cap)).status == CheckResult::unsat);
    const auto too_many = bv_solver::check({}, parse("(not " + at_cap + ")"));
    assert(too_many.status == CheckResult::unknown && too_many.model.empty());
    assert(too_many.reason == "BV expression limit exceeded");

    std::string deep;
    for (unsigned i = 0; i < 256; ++i) deep += "(not ";
    deep += "true" + std::string(256, ')');
    assert(bv_solver::check({}, parse(deep)).status == CheckResult::sat);
    const auto too_deep = bv_solver::check({}, parse("(not " + deep + ")"));
    assert(too_deep.status == CheckResult::unknown && too_deep.model.empty());
    assert(too_deep.reason == "BV expression limit exceeded");
}

void source_witness_validation() {
    const bv_solver::Environment env{{"x", 2}, {"y", 2}};
    const bv_solver::Model model{{"x", 0}, {"y", 1}};
    const auto term = parse("(= (bvsub x y) y)").front();
    bv_solver::detail::Program tape;
    assert(bv_solver::detail::compile(term, env, tape) == 0);
    std::vector<std::uint64_t> stack;
    assert(bv_solver::detail::run(tape, model, stack) == 0); // (0 - 1) mod 4 >= 1
    // Replay of a faulty tape with reversed subtraction operands accepts
    // this false witness; evaluation of the source still rejects it.
    std::swap(tape[0], tape[1]);
    assert(bv_solver::detail::run(tape, model, stack) == 1);
    bv_solver::detail::TypedValue checked;
    sat_detail::WorkBudget work{tape.size()};
    assert(bv_solver::detail::evaluate_source(term, env, model, checked, work));
    assert(checked.width == 0 && checked.value == 0 && work.remaining == 0);
}

void session_commands() {
    Session s;
    auto replies = s.execute("(set-logic QF_BV) (declare-const |x y| (_ BitVec 3))"
                             "(declare-fun b () Bool) (assert (and b (= (bvadd |x y| #b001) #b000)))"
                             "(check-sat) (get-model) (get-value ((bvnot |x y|) b))");
    assert(replies.size() == 3 && replies[0].text == "sat");
    assert(replies[1].text.find("|x y| () (_ BitVec 3) #b111") != std::string::npos);
    assert(replies[1].text.find("b () Bool true") != std::string::npos);
    assert(replies[2].text.find("#b000") != std::string::npos);
    const auto& x = std::get<BVModelResponse>(replies[1].payload).bindings.at("x y");
    assert(x.width == 3 && x.value == 7);
    const auto& values = std::get<BVValuesResponse>(replies[2].payload).bindings;
    assert(values.at(0).second.width == 3 && values.at(0).second.value == 0);
    assert(values.at(1).second.width == 0 && values.at(1).second.value == 1);
    assert(s.last_bv_result()->model.at("x y") == 7);
    assert(s.last_result().status == CheckResult::unknown);
    for (const auto* bad : {"(assert #b0)", "(assert (= b #b1))",
                            "(assert (= (bvadd |x y| #b11) #b000))",
                            "(assert (= (bvudiv |x y| #b000) |x y|))",
                            "(declare-const z (_ BitVec 65))"}) {
        bool rejected = false;
        try { s.execute(bad); } catch (const Error&) { rejected = true; }
        assert(rejected);
        assert(s.execute("(get-model)").at(0).text == replies[1].text);
    }
    replies = s.execute("(push 1) (assert (= |x y| #b000)) (check-sat) (pop 1) (check-sat)");
    assert(replies.size() == 2 && replies[0].text == "unsat" && replies[1].text == "sat");
    replies = s.execute("(declare-const wide (_ BitVec 17)) (assert (= wide wide)) (check-sat)");
    assert(replies.at(0).text == "unknown");
}
}

int main() {
    exhaustive_small_values();
    search_and_budget();
    width_and_input_limits();
    source_witness_validation();
    session_commands();
}
