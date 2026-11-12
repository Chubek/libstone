#pragma once
#include "Supersonic.hpp"

namespace moonstone::supersonic {

// Legacy sampling callback: absence of a counterexample is NOT proof.
using CounterexampleOracle = std::function<std::optional<Example>(const Expr &)>;
struct Verification {
    ProofResult proof;
    std::optional<Example> counterexample;
};
using VerificationOracle = std::function<Verification(const Expr &)>;
struct CegisSolution {
    Solution solution;
    std::size_t iterations = 0;
    bool fully_verified = false;
    ProofResult proof;
};

inline CegisSolution cegis_verified(Problem problem, const VerificationOracle &oracle,
                                    std::size_t max_iterations = 64) {
    CegisSolution result;
    if (!oracle) {
        result.proof = {Status::InvalidInput, "missing verification oracle", {}, 0};
        return result;
    }
    auto invalid = [&]() {
        result.solution.status = SolveStatus::InvalidInput;
        result.proof = {
            Status::InvalidInput, "inconsistent oracle response or invalid counterexample", {}, 0};
        return result;
    };
    for (std::size_t step = 0; step < max_iterations; ++step) {
        result.solution = solve(problem);
        result.iterations = step + 1;
        if (result.solution.status != SolveStatus::Found) {
            auto status =
                result.solution.status == SolveStatus::BudgetExhausted ? Status::BudgetExhausted
                : result.solution.status == SolveStatus::InvalidInput  ? Status::InvalidInput
                                                                       : Status::Unknown;
            result.proof = {status, "no candidate verified; see bounded synthesis status", {}, 0};
            return result;
        }
        auto verification = oracle(*result.solution.program);
        result.proof = std::move(verification.proof);
        if (result.proof.status == Status::Proven) {
            if (verification.counterexample)
                return invalid();
            result.fully_verified = true; // Subject to the preserved oracle assumptions.
            return result;
        }
        if (result.proof.status != Status::Refuted) {
            if (verification.counterexample)
                return invalid();
            return result; // UNKNOWN/exhaustion is not success.
        }
        if (!verification.counterexample)
            return invalid();
        const auto &failing = *verification.counterexample;
        for (const auto &variable : problem.grammar.variables) {
            if (!failing.inputs.count(variable))
                return invalid();
        }
        auto actual = evaluate(*result.solution.program, failing.inputs);
        if (actual && *actual == failing.expected)
            return invalid();
        for (const auto &example : problem.examples) {
            if (example.inputs == failing.inputs)
                return invalid();
        }
        problem.examples.push_back(failing);
    }
    result.solution.status = SolveStatus::BudgetExhausted;
    result.proof = {Status::BudgetExhausted, "CEGIS iteration limit", {}, max_iterations};
    return result;
}

inline CegisSolution cegis(Problem problem, const CounterexampleOracle &oracle,
                           std::size_t max_iterations = 64) {
    if (!oracle)
        return cegis_verified(std::move(problem), VerificationOracle{}, max_iterations);
    VerificationOracle provisional = [&](const Expr &expression) {
        auto failing = oracle(expression);
        ProofResult proof{failing ? Status::Refuted : Status::Unknown,
                          failing ? "counterexample supplied"
                                  : "legacy oracle returned no counterexample; not a proof",
                          {},
                          0};
        return Verification{std::move(proof), std::move(failing)};
    };
    return cegis_verified(std::move(problem), provisional, max_iterations);
}

} // namespace moonstone::supersonic
