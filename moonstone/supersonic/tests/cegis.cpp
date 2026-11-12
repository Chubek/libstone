#include <cassert>
#include <supersonic/CEGIS.hpp>

using namespace moonstone;
using namespace moonstone::supersonic;

// Independent test-only interpreter. The test grammar has constants 0/1, x in
// [-5,5], and at most six nodes, so these direct integer operations cannot
// overflow. This verifies only that explicitly finite test domain.
static std::int64_t independent(const Expr &expression, std::int64_t x) {
    switch (expression.op) {
    case Op::Constant:
        return expression.value;
    case Op::Variable:
        assert(expression.variable == "x");
        return x;
    case Op::Neg:
        return -independent(expression.args.at(0), x);
    case Op::Add:
        return independent(expression.args.at(0), x) + independent(expression.args.at(1), x);
    case Op::Sub:
        return independent(expression.args.at(0), x) - independent(expression.args.at(1), x);
    case Op::Mul:
        return independent(expression.args.at(0), x) * independent(expression.args.at(1), x);
    }
    throw std::invalid_argument("unsupported test expression");
}

int main() {
    Problem problem;
    problem.grammar.variables = {"x"};
    problem.grammar.constants = {0, 1};
    problem.examples = {{{{"x", 0}}, 1}};
    auto sampled =
        cegis(problem, [](const Expr &) -> std::optional<Example> { return std::nullopt; });
    assert(sampled.solution.status == SolveStatus::Found);
    assert(!sampled.fully_verified && sampled.proof.status == Status::Unknown);
    // The independently specified finite domain is the scope of this proof.
    VerificationOracle finite = [](const Expr &expression) -> Verification {
        for (std::int64_t x = -5; x <= 5; ++x) {
            auto value = independent(expression, x);
            if (value != x + 1) {
                return {{Status::Refuted, "finite-domain counterexample", {}, 11},
                        Example{{{"x", x}}, x + 1}};
            }
        }
        return {{Status::Proven, "exhaustive finite-domain check", {"x in [-5,5] only"}, 11},
                std::nullopt};
    };
    auto verified = cegis_verified(problem, finite);
    assert(verified.fully_verified && verified.solution.program && verified.iterations > 1);
    assert(verified.proof.assumptions == std::vector<std::string>{"x in [-5,5] only"});
    for (auto status : {Status::Unknown, Status::BudgetExhausted, Status::InvalidInput}) {
        auto stopped =
            cegis_verified(problem, VerificationOracle{[=](const Expr &) -> Verification {
                               return {{status, "not verified", {}, 0}, std::nullopt};
                           }});
        assert(!stopped.fully_verified && stopped.proof.status == status);
    }
    auto exhausted = cegis_verified(problem, finite, 0);
    assert(exhausted.solution.status == SolveStatus::BudgetExhausted && !exhausted.fully_verified);
    exhausted = cegis_verified(problem, finite, 1);
    assert(exhausted.proof.status == Status::BudgetExhausted && !exhausted.fully_verified);
    auto missing = cegis_verified(problem, VerificationOracle{});
    assert(missing.solution.status == SolveStatus::InvalidInput && !missing.fully_verified);
    for (auto invalid : {Verification{{Status::Refuted, "", {}, 0}, std::nullopt},
                         Verification{{Status::Proven, "", {}, 0}, Example{{{"x", 0}}, 1}},
                         Verification{{Status::Refuted, "", {}, 0}, Example{{}, 0}},
                         Verification{{Status::Refuted, "", {}, 0},
                                      Example{{{"x", 2}}, 1}}, // Doesn't refute constant 1.
                         Verification{{Status::Refuted, "", {}, 0},
                                      Example{{{"x", 0}}, 2}}, // Contradicts original example.
                         Verification{{Status::Unknown, "", {}, 0}, Example{{{"x", 2}}, 3}}}) {
        auto rejected =
            cegis_verified(problem, VerificationOracle{[=](const Expr &) { return invalid; }});
        assert(!rejected.fully_verified && rejected.proof.status == Status::InvalidInput);
    }
}
