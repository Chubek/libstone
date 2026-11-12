#pragma once
#include "../../../core/CheckedArithmetic.hpp"
#include "../../../core/Core.hpp"
#include <unordered_set>

namespace moonstone::supersonic {

enum class Op { Constant, Variable, Add, Sub, Mul, Neg };

struct Expr {
    Op op = Op::Constant;
    std::int64_t value = 0;
    std::string variable;
    std::vector<Expr> args;

    std::string print() const {
        switch (op) {
        case Op::Constant:
            if (!args.empty())
                break;
            return std::to_string(value);
        case Op::Variable:
            if (!args.empty() || variable.empty())
                break;
            return variable;
        case Op::Neg:
            if (args.size() != 1)
                break;
            return "(-" + args[0].print() + ")";
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
            if (args.size() != 2)
                break;
            return "(" + args[0].print() +
                   (op == Op::Add   ? "+"
                    : op == Op::Sub ? "-"
                                    : "*") +
                   args[1].print() + ")";
        }
        throw std::invalid_argument("malformed or unsupported synthesis expression");
    }
};

inline std::optional<std::int64_t> checked(Op op, std::int64_t a, std::int64_t b = 0) {
    switch (op) {
    case Op::Add:
        return arithmetic::add(a, b);
    case Op::Sub:
        return arithmetic::sub(a, b);
    case Op::Mul:
        return arithmetic::mul(a, b);
    case Op::Neg:
        return arithmetic::neg(a);
    default:
        return std::nullopt;
    }
}

namespace detail {
inline std::optional<std::int64_t> evaluate(const Expr &e,
                                            const std::map<std::string, std::int64_t> &vars,
                                            std::size_t depth, std::size_t &nodes) {
    if (depth == 0 || nodes == 0)
        return std::nullopt;
    --nodes;
    switch (e.op) {
    case Op::Constant:
        return e.args.empty() ? std::optional{e.value} : std::nullopt;
    case Op::Variable: {
        auto it = vars.find(e.variable);
        return e.args.empty() && it != vars.end() ? std::optional{it->second} : std::nullopt;
    }
    case Op::Neg:
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
        break;
    default:
        return std::nullopt;
    }
    if (e.args.size() != (e.op == Op::Neg ? 1u : 2u))
        return std::nullopt;
    auto a = evaluate(e.args[0], vars, depth - 1, nodes);
    if (!a)
        return std::nullopt;
    if (e.op == Op::Neg)
        return checked(e.op, *a);
    auto b = evaluate(e.args[1], vars, depth - 1, nodes);
    return b ? checked(e.op, *a, *b) : std::nullopt;
}

inline bool identifier(std::string_view value) {
    auto first = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (value.empty() || !first(value.front()))
        return false;
    for (char c : value)
        if (!first(c) && !(c >= '0' && c <= '9'))
            return false;
    return true;
}
} // namespace detail

// Failure includes malformed/unsupported terms, overflow, absent variables and
// evaluator limits. It is never evidence that an expression is safe.
inline std::optional<std::int64_t> evaluate(const Expr &e,
                                            const std::map<std::string, std::int64_t> &vars,
                                            std::size_t max_depth = 256,
                                            std::size_t max_nodes = 100000) {
    return detail::evaluate(e, vars, max_depth, max_nodes);
}

struct Example {
    std::map<std::string, std::int64_t> inputs;
    std::int64_t expected = 0;
};
struct Grammar {
    std::vector<std::string> variables;
    std::vector<std::int64_t> constants;
    std::vector<Op> operations{Op::Add, Op::Sub, Op::Mul};
};
struct Budget {
    std::size_t max_cost = 6, max_candidates = 100000;
};
struct Problem {
    Grammar grammar;
    std::vector<Example> examples;
    Budget budget;
    std::function<bool(const Expr &)> predicate;
};
enum class SolveStatus { Found, UnsatisfiableWithinBound, BudgetExhausted, InvalidInput };
struct Solution {
    SolveStatus status = SolveStatus::InvalidInput;
    std::optional<Expr> program;
    std::size_t candidates = 0;
};

inline Solution solve(const Problem &p) {
    if (p.examples.empty() || p.budget.max_cost == 0 || p.budget.max_cost > 12 ||
        p.budget.max_candidates == 0)
        return {};
    std::set<std::string> variables;
    for (const auto &variable : p.grammar.variables) {
        if (!detail::identifier(variable) || !variables.insert(variable).second)
            return {};
        for (const auto &example : p.examples)
            if (!example.inputs.count(variable))
                return {};
    }
    for (auto op : p.grammar.operations) {
        if (op != Op::Add && op != Op::Sub && op != Op::Mul && op != Op::Neg)
            return {};
    }
    Solution result;
    std::vector<std::vector<Expr>> level(p.budget.max_cost + 1);
    std::unordered_set<std::string> seen;
    // Returns true on either success or exhaustion, stopping all enumeration.
    auto add = [&](std::size_t cost, Expr expression) {
        auto representation = expression.print();
        if (seen.count(representation))
            return false;
        if (result.candidates == p.budget.max_candidates) {
            result.status = SolveStatus::BudgetExhausted;
            return true;
        }
        seen.insert(std::move(representation));
        ++result.candidates;
        level[cost].push_back(std::move(expression));
        const auto &candidate = level[cost].back();
        for (const auto &example : p.examples) {
            auto value = evaluate(candidate, example.inputs);
            if (!value || *value != example.expected)
                return false;
        }
        if (p.predicate && !p.predicate(candidate))
            return false;
        result.status = SolveStatus::Found;
        result.program = candidate;
        return true;
    };
    for (auto constant : p.grammar.constants) {
        if (add(1, Expr{Op::Constant, constant, {}, {}}))
            return result;
    }
    for (const auto &variable : p.grammar.variables) {
        if (add(1, Expr{Op::Variable, 0, variable, {}}))
            return result;
    }
    for (std::size_t cost = 2; cost <= p.budget.max_cost; ++cost) {
        for (auto op : p.grammar.operations) {
            if (op == Op::Neg) {
                for (const auto &a : level[cost - 1]) {
                    if (add(cost, Expr{op, 0, {}, {a}}))
                        return result;
                }
                continue;
            }
            for (std::size_t left = 1; left + 1 < cost; ++left) {
                auto right = cost - left - 1;
                for (const auto &a : level[left]) {
                    for (const auto &b : level[right]) {
                        // A syntax-sensitive predicate may distinguish a+b from b+a.
                        if (!p.predicate && (op == Op::Add || op == Op::Mul) &&
                            a.print() > b.print())
                            continue;
                        if (add(cost, Expr{op, 0, {}, {a, b}}))
                            return result;
                    }
                }
            }
        }
    }
    result.status = SolveStatus::UnsatisfiableWithinBound;
    return result;
}

} // namespace moonstone::supersonic
