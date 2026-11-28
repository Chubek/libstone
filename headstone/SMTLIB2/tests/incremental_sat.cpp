#ifdef NDEBUG
#undef NDEBUG
#endif
#include <smtlib2/incremental_sat.hpp>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

using namespace smtlib2;

namespace {
constexpr auto no_failure = std::numeric_limits<std::size_t>::max();
std::size_t allocations_until_failure = no_failure;

struct FailAllocation {
    explicit FailAllocation(std::size_t successful) { allocations_until_failure = successful; }
    ~FailAllocation() { allocations_until_failure = no_failure; }
};
}

// This test executable is single-threaded. Inject failures without changing
// the public allocator or solver API.
void* operator new(std::size_t size) {
    if (allocations_until_failure != no_failure && allocations_until_failure-- == 0)
        throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static SExpr term(std::string_view text) { return parse(text).at(0); }

static void expect(SolveResult result, CheckResult status) {
    assert(result.status == status);
    if (status != CheckResult::sat) assert(result.model.empty());
}

static void test_scopes_and_assumptions() {
    IncrementalBoolSolver solver({"a", "b", "unused"});
    solver.assert_formula(term("(or a b)"));
    assert(solver.encoded_assertion_count() == 0);
    auto result = solver.check(parse("(not a)"));
    expect(result, CheckResult::sat);
    assert(!result.model.at("a") && result.model.at("b"));
    assert(result.model.count("unused") == 1);
    assert(solver.encoded_assertion_count() == 1);

    solver.push();
    solver.assert_formula(term("(not b)"));
    solver.push();
    solver.assert_formula(term("(not a)"));
    expect(solver.check(), CheckResult::unsat);
    assert(solver.encoded_assertion_count() == 3);
    assert(!solver.pop(3));
    assert(solver.pop(0));
    assert(solver.encoded_assertion_count() == 3);
    expect(solver.check(), CheckResult::unsat);
    assert(solver.pop());
    assert(solver.encoded_assertion_count() == 2);
    expect(solver.check(parse("(not a)")), CheckResult::unsat);
    result = solver.check();
    expect(result, CheckResult::sat);
    assert(result.model.at("a") && !result.model.at("b"));

    solver.push();
    solver.push();
    solver.assert_formula(term("false"));
    assert(solver.pop(3)); // Includes an empty frame and an unchecked assertion.
    assert(solver.encoded_assertion_count() == 1);
    assert(!solver.pop());
    expect(solver.check(parse("(not a)")), CheckResult::sat);
    expect(solver.check(parse("(not a) (not b)")), CheckResult::unsat);
    expect(solver.check(), CheckResult::sat);
    assert(solver.encoded_assertion_count() == 1);
}

static void test_resources_and_failed_encodings() {
    SatLimits variable_cap;
    variable_cap.max_cnf_variables = 4;
    IncrementalBoolSolver solver({"a", "b"}, variable_cap);
    solver.assert_formula(term("(and a b)"));
    expect(solver.check(), CheckResult::sat);
    assert(solver.encoded_assertion_count() == 1);

    // A failed assumption after a prior temporary allocation must unwind both.
    expect(solver.check(parse("true (or a b)")), CheckResult::unknown);
    expect(solver.check(parse("(or a b)")), CheckResult::sat);
    expect(solver.check(parse("false")), CheckResult::unsat);
    expect(solver.check(parse("true (f a)")), CheckResult::unknown);
    expect(solver.check(parse("true missing")), CheckResult::unknown);
    expect(solver.check(), CheckResult::sat);

    // Failed encoding of the outer gate must not leak the inner gate.
    solver.push();
    solver.assert_formula(term("(and a (or a b))"));
    expect(solver.check(), CheckResult::unknown);
    assert(solver.encoded_assertion_count() == 1);
    expect(solver.check(), CheckResult::unknown);
    assert(solver.pop());
    for (int i = 0; i != 32; ++i) {
        solver.push();
        solver.assert_formula(term("(or a b)"));
        expect(solver.check(), CheckResult::sat);
        assert(solver.encoded_assertion_count() == 2);
        assert(solver.pop());
        assert(solver.encoded_assertion_count() == 1);
    }

    solver.push();
    solver.assert_formula(term("false"));
    solver.assert_formula(term("(unsupported a)"));
    expect(solver.check(), CheckResult::unknown);
    assert(solver.encoded_assertion_count() == 2);
    assert(solver.pop());
    expect(solver.check(parse("true")), CheckResult::sat);

    SatLimits literal_cap;
    literal_cap.max_cnf_literals = 8;
    IncrementalBoolSolver limited_literals({"a", "b"}, literal_cap);
    expect(limited_literals.check(parse("(xor a b)")), CheckResult::unknown);
    expect(limited_literals.check(parse("(and a b)")), CheckResult::sat);
    expect(limited_literals.check(parse("(and a b)")), CheckResult::sat);
    IncrementalBoolSolver capped_roots({"a"}, literal_cap);
    expect(capped_roots.check(parse("a a a a a a a a a")), CheckResult::unknown);
    expect(capped_roots.check(parse("a")), CheckResult::sat);

    SatLimits depth_cap;
    depth_cap.max_depth = 1;
    IncrementalBoolSolver limited_depth({"a"}, depth_cap);
    expect(limited_depth.check(parse("(not (not a))")), CheckResult::unknown);
    expect(limited_depth.check(parse("(not a)")), CheckResult::sat);

    SatLimits work_cap;
    work_cap.max_work = 0;
    IncrementalBoolSolver limited_work({}, work_cap);
    limited_work.assert_formula(term("true"));
    expect(limited_work.check(), CheckResult::unknown);

    work_cap.max_work = 5;
    IncrementalBoolSolver limited_search({"a", "b"}, work_cap);
    limited_search.assert_formula(term("(and a b)"));
    expect(limited_search.check(), CheckResult::unknown);
    assert(limited_search.encoded_assertion_count() == 1);
    expect(limited_search.check(), CheckResult::unknown);

    work_cap.max_work = 20;
    IncrementalBoolSolver temporary_work({"a"}, work_cap);
    temporary_work.assert_formula(term("a"));
    for (int i = 0; i != 32; ++i) {
        expect(temporary_work.check(parse("a a a a a a a a a a a a a a a a")), CheckResult::unknown);
        expect(temporary_work.check(), CheckResult::sat);
        assert(temporary_work.encoded_assertion_count() == 1);
    }

    expect(IncrementalBoolSolver({"a"}, {}, 0).check(), CheckResult::unknown);
    expect(IncrementalBoolSolver({"true"}).check(), CheckResult::unknown);
}

static void test_differential() {
    const std::vector<std::string> names{"a", "b", "c", "quoted atom"};
    const auto formulas = parse(
        "a (not a) (and a b c) (or a b c) (xor a b c) (=> a b c) (= a b c) "
        "(distinct a b) (distinct a b c) (ite a b c) (and |quoted atom| a) "
        "(ite true false c) true false");
    const ReferenceBoolSolver reference;
    IncrementalBoolSolver solver(names);
    for (std::size_t i = 0; i != formulas.size(); ++i) {
        solver.push();
        solver.assert_formula(formulas[i]);
        for (std::size_t j = 0; j != formulas.size(); ++j) {
            solver.push();
            solver.assert_formula(formulas[j]);
            const auto& assumption = formulas[(i + j) % formulas.size()];
            const auto result = solver.check({assumption});
            const std::vector<SExpr> originals{formulas[i], formulas[j], assumption};
            expect(result, reference.check(names, originals).status);
            if (result.status == CheckResult::sat) {
                assert(result.model.size() == names.size());
                for (const auto& original : originals) {
                    bool holds = false;
                    assert(BoolSolver::evaluate(original, result.model, holds) && holds);
                }
            }
            assert(solver.pop());
            assert(solver.encoded_assertion_count() == 1);
        }
        assert(solver.pop());
        assert(solver.encoded_assertion_count() == 0);
    }
}

static void test_copy_and_move_with_cached_scopes() {
    IncrementalBoolSolver original({"a", "b"});
    original.assert_formula(term("(or a b)"));
    original.push();
    original.assert_formula(term("(not a)"));
    expect(original.check(parse("(not b)")), CheckResult::unsat);
    assert(original.encoded_assertion_count() == 2);

    auto copy = original;
    assert(copy.pop() && copy.encoded_assertion_count() == 1);
    expect(copy.check(parse("(not b)")), CheckResult::sat);
    expect(original.check(parse("(not b)")), CheckResult::unsat);

    auto moved = std::move(original);
    assert(original.encoded_assertion_count() == 0 && !original.pop());
    expect(original.check(), CheckResult::unknown);
    assert(moved.encoded_assertion_count() == 2);
    original = std::move(moved);
    assert(!moved.pop() && moved.encoded_assertion_count() == 0);
    assert(original.pop() && original.encoded_assertion_count() == 1);
    expect(original.check(parse("(not b)")), CheckResult::sat);
}

static void test_atomic_scope_push_and_check() {
    IncrementalBoolSolver initial({"a", "b"});
    initial.assert_formula(term("a"));
    expect(initial.check(), CheckResult::sat);
    initial.push_scopes(0);
    assert(!initial.pop());
    try {
        const FailAllocation fail(0);
        initial.push_scopes(2);
        assert(false);
    } catch (const std::bad_alloc&) {}
    assert(!initial.pop());
    initial.push_scopes(2);
    initial.assert_formula(term("(or b (not a))"));
    try {
        initial.push_scopes(no_failure);
        assert(false);
    } catch (const std::length_error&) {}
    assert(!initial.pop(3));

    const auto assumptions = parse("(and a b)");
    unsigned failures = 0;
    for (std::size_t n = 0; ; ++n) {
        assert(n < 256);
        auto solver = initial;
        try {
            const FailAllocation fail(n);
            expect(solver.check(assumptions), CheckResult::sat);
        } catch (const std::bad_alloc&) {
            ++failures;
            assert(solver.encoded_assertion_count() == 1);
            expect(solver.check(assumptions), CheckResult::sat);
            assert(solver.encoded_assertion_count() == 2);
            assert(solver.pop(2) && !solver.pop());
            expect(solver.check(parse("(not b)")), CheckResult::sat);
            continue;
        }
        assert(solver.encoded_assertion_count() == 2);
        break;
    }
    assert(failures > 1);
}

static void test_atomic_copy_assignment() {
    IncrementalBoolSolver source({"a", "b"});
    source.assert_formula(term("(or a b)"));
    source.push();
    source.assert_formula(term("(not a)"));
    expect(source.check(), CheckResult::sat);
    const auto not_b = parse("(not b)");

    unsigned failures = 0;
    for (std::size_t n = 0; ; ++n) {
        assert(n < 256);
        IncrementalBoolSolver target({"a", "b"});
        target.assert_formula(term("(not a)"));
        expect(target.check(), CheckResult::sat);
        try {
            const FailAllocation fail(n);
            target = source;
        } catch (const std::bad_alloc&) {
            ++failures;
            assert(target.encoded_assertion_count() == 1);
            assert(!target.pop());
            expect(target.check(not_b), CheckResult::sat);
            continue;
        }
        assert(target.encoded_assertion_count() == 2);
        expect(target.check(not_b), CheckResult::unsat);
        assert(target.pop() && !target.pop());
        expect(target.check(not_b), CheckResult::sat);
        break;
    }
    assert(failures > 1);
}

int main() {
    test_scopes_and_assumptions();
    test_resources_and_failed_encodings();
    test_differential();
    test_copy_and_move_with_cached_scopes();
    test_atomic_scope_push_and_check();
    test_atomic_copy_assignment();
}
