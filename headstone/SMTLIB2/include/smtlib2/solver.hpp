#pragma once
#include "sexpr.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smtlib2 {
enum class CheckResult { sat, unsat, unknown };
inline const char* to_string(CheckResult result) noexcept { return result == CheckResult::sat ? "sat" : result == CheckResult::unsat ? "unsat" : "unknown"; }
using BoolModel = std::map<std::string, bool>;
struct SolveResult { CheckResult status = CheckResult::unknown; BoolModel model; };

struct SatLimits {
    std::size_t max_cnf_variables = 262144;
    // Includes assertion/assumption units as well as definitional clauses.
    std::size_t max_cnf_literals = 2097152;
    // Counts accepted declarations, encoded/validated term nodes, examined
    // CNF/root literals, backtracked assignments, and projected bindings.
    // This is a structural budget, not a byte or elapsed-time limit.
    std::size_t max_work = 10000000;
    // Counts nested applications; additionally capped at 512 for stack safety.
    std::size_t max_depth = 512;
};

namespace sat_detail {
inline constexpr std::size_t default_max_variables = 65536;
inline constexpr std::size_t safe_depth = 512;

struct WorkBudget {
    std::size_t remaining;

    bool spend(std::size_t amount = 1) noexcept {
        if (amount > remaining) return false;
        remaining -= amount;
        return true;
    }
};

inline std::optional<std::string_view> symbol(const SExpr& e) {
    if (e.is_list || !e.children.empty()) return {};
    if (e.kind == TokenKind::atom && !e.atom.empty()) return std::string_view(e.atom);
    if (e.kind == TokenKind::quoted_symbol && e.atom.size() >= 2 &&
        e.atom.front() == '|' && e.atom.back() == '|')
        return std::string_view(e.atom).substr(1, e.atom.size() - 2);
    return {};
}

enum class Op { invalid, not_, and_, or_, xor_, implies, equal, distinct, ite };
inline Op operation_name(std::string_view name) {
    if (name == "not") return Op::not_;
    if (name == "ite") return Op::ite;
    if (name == "and") return Op::and_;
    if (name == "or") return Op::or_;
    if (name == "xor") return Op::xor_;
    if (name == "=>") return Op::implies;
    if (name == "=") return Op::equal;
    if (name == "distinct") return Op::distinct;
    return Op::invalid;
}

inline bool reserved(std::string_view name) {
    return name == "true" || name == "false" || operation_name(name) != Op::invalid;
}

inline Op operation(const SExpr& e) {
    if (!e.is_list || !e.atom.empty() || e.children.empty()) return Op::invalid;
    const auto name = symbol(e.children.front());
    if (!name) return Op::invalid;
    const auto op = operation_name(*name);
    const auto n = e.children.size() - 1;
    return (op == Op::not_ ? n == 1 : op == Op::ite ? n == 3 : n >= 2) ? op : Op::invalid;
}

// Independent direct semantics used both by the enumerator and to verify
// the projected SAT assignment. Every operand is visited even if it cannot
// affect the value, so an unsupported term cannot be hidden by short-circuiting.
inline bool evaluate(const SExpr& e, const BoolModel& model, bool& result,
                     std::size_t depth, WorkBudget* work = nullptr) {
    if (work && !work->spend()) return false;
    if (!e.is_list) {
        const auto name = symbol(e);
        if (!name) return false;
        if (name == "true" || name == "false") { result = name == "true"; return true; }
        if (operation_name(*name) != Op::invalid) return false;
        auto it = model.find(std::string(*name));
        if (it == model.end()) return false;
        result = it->second;
        return true;
    }
    const auto op = operation(e);
    if (op == Op::invalid || depth == 0) return false;
    auto operand = [&](std::size_t i, bool& value) {
        return evaluate(e.children[i], model, value, depth - 1, work);
    };
    bool first;
    if (!operand(1, first)) return false;
    if (op == Op::not_) { result = !first; return true; }
    if (op == Op::ite) {
        bool on_true, on_false;
        if (!operand(2, on_true) || !operand(3, on_false)) return false;
        result = first ? on_true : on_false;
        return true;
    }
    const auto last = e.children.size() - 1;
    bool value = op == Op::equal ? true : op == Op::implies ? !first : first;
    for (std::size_t i = 2; i <= last; ++i) {
        bool arg;
        if (!operand(i, arg)) return false;
        switch (op) {
        case Op::and_: value = value && arg; break;
        case Op::or_: value = value || arg; break;
        case Op::xor_: value = value != arg; break;
        case Op::implies: value = value || (i == last ? arg : !arg); break;
        case Op::equal: value = value && (arg == first); break;
        case Op::distinct: value = last == 2 && (arg != first); break;
        default: return false;
        }
    }
    result = value;
    return true;
}

using Literal = int; // Positive or negative variable ID; zero signals failure.
// Tseitin gates emit clauses of at most three literals. Store them inline
// with zero padding to avoid a separate allocation for every clause.
using Clause = std::array<Literal, 3>;
inline std::size_t variable(Literal lit) {
    return static_cast<std::size_t>(lit < 0 ? -lit : lit);
}

// An append-only encoding with stack-discipline checkpoints. Search is local
// to each solve call; assertion units are supplied separately from the gates.
class Cnf {
public:
    struct Checkpoint {
    private:
        friend class Cnf;
        std::size_t clauses, symbols, literals_left, work_left;
        Literal variables, truth;

        Checkpoint(std::size_t clauses, std::size_t symbols, std::size_t literals_left,
                   std::size_t work_left, Literal variables, Literal truth) noexcept
            : clauses(clauses), symbols(symbols), literals_left(literals_left),
              work_left(work_left), variables(variables), truth(truth) {}
    };

    explicit Cnf(SatLimits bounds)
        : limits_(bounds), literals_left_(bounds.max_cnf_literals), work_{bounds.max_work} {}

    Checkpoint checkpoint() const noexcept {
        return {clauses_.size(), symbol_order_.size(), literals_left_, work_.remaining, variables_, truth_};
    }

    // The checkpoint must belong to the current encoding history, not a
    // branch that has already been discarded.
    void rollback(Checkpoint checkpoint) noexcept {
        while (clauses_.size() > checkpoint.clauses) clauses_.pop_back();
        while (symbol_order_.size() > checkpoint.symbols) {
            symbols_.erase(symbols_.find(symbol_order_.back()));
            symbol_order_.pop_back();
        }
        literals_left_ = checkpoint.literals_left;
        work_.remaining = checkpoint.work_left;
        variables_ = checkpoint.variables;
        truth_ = checkpoint.truth;
    }

    bool declare_symbol(std::string_view name, std::size_t max_variables = default_max_variables) {
        if (reserved(name) || work_.remaining == 0) return false;
        if (symbols_.find(name) != symbols_.end()) return work_.spend();
        if (symbols_.size() >= max_variables || !can_allocate_variable()) return false;
        symbol_order_.emplace_back(name);
        try {
            symbols_.emplace(symbol_order_.back(), variables_ + 1);
        } catch (...) {
            symbol_order_.pop_back();
            throw;
        }
        ++variables_;
        work_.spend();
        return true;
    }

    // Reserve room for the root unit as well as encoding the root expression.
    // A failed encoding leaves the active CNF and construction budget intact.
    Literal encode_root(const SExpr& term) {
        if (literals_left_ == 0) return 0;
        const auto saved = checkpoint();
        try {
            --literals_left_;
            if (const auto root = encode(term, depth())) return root;
        } catch (...) {
            rollback(saved);
            throw;
        }
        rollback(saved);
        return 0;
    }

    // Roots returned by encode_root and their original terms must correspond
    // and remain in the active encoding history. No clauses or search state
    // change here; repeated checks start with the same remaining work budget.
    SolveResult solve(const std::vector<Literal>& roots, const std::vector<const SExpr*>& assertions) const {
        // Every root must be visited, even for an immediate contradiction.
        // Avoid allocating search state when that work cannot fit.
        if (roots.size() != assertions.size() || roots.size() > work_.remaining) return {};
        auto work = work_;
        std::vector<signed char> values;
        const auto status = search(roots, assertions, values, work);
        if (status != CheckResult::sat) return {status, {}};
        BoolModel model;
        for (const auto& entry : symbols_) {
            if (!work.spend()) return {};
            // Unassigned variables can be completed arbitrarily: each clause
            // is already witnessed by a literal assigned true.
            model.emplace_hint(model.end(), entry.first, values[variable(entry.second)] > 0);
        }
        for (const auto* a : assertions) {
            bool value;
            if (!evaluate(*a, model, value, depth(), &work) || !value) return {};
        }
        return {CheckResult::sat, std::move(model)};
    }

private:
    std::size_t depth() const noexcept {
        return std::min(limits_.max_depth, safe_depth);
    }

    bool can_allocate_variable() const noexcept {
        return variable(variables_) < limits_.max_cnf_variables &&
               variables_ < std::numeric_limits<Literal>::max();
    }

    Literal fresh() noexcept {
        return can_allocate_variable() ? ++variables_ : 0;
    }

    bool add(Clause clause) {
        const std::size_t size = clause[2] ? 3 : clause[1] ? 2 : 1;
        if (size > literals_left_) return false;
        clauses_.push_back(clause);
        literals_left_ -= size;
        return true;
    }

    Literal constant(bool value) {
        if (!truth_) {
            truth_ = fresh();
            if (!truth_ || !add({truth_})) return 0;
        }
        return value ? truth_ : -truth_;
    }

    // Every gate is constrained in both directions; literals can be signed.
    Literal conjunction(Literal a, Literal b) {
        const auto z = fresh();
        return z && add({-z, a}) && add({-z, b}) && add({z, -a, -b}) ? z : 0;
    }

    Literal exclusive_or(Literal a, Literal b) {
        const auto z = fresh();
        return z && add({-z, a, b}) && add({-z, -a, -b}) &&
               add({z, -a, b}) && add({z, a, -b}) ? z : 0;
    }

    Literal encode(const SExpr& e, std::size_t depth) {
        if (!work_.spend()) return 0;
        if (!e.is_list) {
            const auto name = symbol(e);
            if (!name) return 0;
            if (name == "true" || name == "false") return constant(name == "true");
            const auto it = symbols_.find(*name);
            return it == symbols_.end() ? 0 : it->second;
        }
        const auto op = operation(e);
        if (op == Op::invalid || depth == 0) return 0;
        if (op == Op::not_) return -encode(e.children[1], depth - 1);
        std::vector<Literal> args;
        for (std::size_t i = 1; i < e.children.size(); ++i) {
            const auto child = encode(e.children[i], depth - 1);
            if (!child) return 0;
            args.push_back(child);
        }
        switch (op) {
        case Op::distinct: return args.size() == 2 ? exclusive_or(args[0], args[1]) : constant(false);
        case Op::ite: {
            const auto c = args[0], t = args[1], f = args[2], z = fresh();
            return z && add({-c, -t, z}) && add({-c, t, -z}) &&
                   add({c, -f, z}) && add({c, f, -z}) ? z : 0;
        }
        case Op::implies: {
            // Right-associative a => b => c is !a || !b || c.
            auto result = args.back();
            for (std::size_t i = args.size() - 1; i != 0 && result; --i)
                result = -conjunction(args[i - 1], -result);
            return result;
        }
        case Op::equal: {
            // Chainable equality, not a left fold of binary equality.
            auto result = -exclusive_or(args[0], args[1]);
            for (std::size_t i = 2; i < args.size() && result; ++i) {
                const auto pair = -exclusive_or(args[0], args[i]);
                if (!pair) return 0;
                result = conjunction(result, pair);
            }
            return result;
        }
        case Op::and_: case Op::or_: case Op::xor_: {
            auto result = args[0];
            for (std::size_t i = 1; i < args.size() && result; ++i) {
                if (op == Op::and_) result = conjunction(result, args[i]);
                else if (op == Op::or_) result = -conjunction(-result, -args[i]);
                else result = exclusive_or(result, args[i]);
            }
            return result;
        }
        default: return 0;
        }
    }

    CheckResult search(const std::vector<Literal>& roots, const std::vector<const SExpr*>& assertions,
                       std::vector<signed char>& assignment,
                       WorkBudget& work) const {
        struct Decision { Literal lit; std::size_t trail_start; bool flipped = false; };
        assignment.assign(variable(variables_) + 1, 0);
        std::vector<Literal> trail;
        std::vector<Decision> decisions;
        auto sign = [](Literal lit) -> signed char { return lit > 0 ? 1 : -1; };
        auto assign = [&](Literal lit) {
            assignment[variable(lit)] = sign(lit);
            trail.push_back(lit);
        };
        // Establish root units once. Their trail entries precede all decisions
        // and therefore survive every backtrack. Validate and charge all root
        // units even if a conflict is already known.
        bool root_conflict = false;
        for (std::size_t i = 0; i < roots.size(); ++i) {
            const auto lit = roots[i];
            if (!work.spend() || !assertions[i] || !lit || lit > variables_ || lit < -variables_)
                return CheckResult::unknown;
            const auto value = assignment[variable(lit)];
            if (value == 0) assign(lit);
            else if (value != sign(lit)) root_conflict = true;
        }
        if (root_conflict) return CheckResult::unsat;
        for (;;) {
            bool conflict = false, changed = false;
            Literal branch = 0;
            for (const auto& clause : clauses_) {
                bool satisfied = false;
                Literal unit = 0;
                bool multiple = false;
                for (const auto lit : clause) {
                    if (!lit) break; // end of this clause's nonzero literals
                    if (!work.spend()) return CheckResult::unknown;
                    const auto value = assignment[variable(lit)];
                    if (value == sign(lit)) { satisfied = true; break; }
                    if (value == 0) {
                        if (!unit) unit = lit;
                        else if (lit != unit) multiple = true;
                    }
                }
                if (satisfied) continue;
                if (!unit) { conflict = true; break; }
                if (!multiple) { assign(unit); changed = true; }
                else if (!branch) branch = unit;
            }
            if (conflict) {
                // Drop decisions whose two branches have both failed.
                while (!decisions.empty()) {
                    auto& d = decisions.back();
                    while (trail.size() > d.trail_start) {
                        if (!work.spend()) return CheckResult::unknown;
                        assignment[variable(trail.back())] = 0;
                        trail.pop_back();
                    }
                    if (!d.flipped) { d.flipped = true; assign(-d.lit); break; }
                    decisions.pop_back();
                }
                if (decisions.empty()) return CheckResult::unsat;
            } else if (!changed) {
                if (!branch) return CheckResult::sat;
                // Choose the first unresolved variable, false first.
                const auto lit = -static_cast<Literal>(variable(branch));
                decisions.push_back({lit, trail.size()});
                assign(lit);
            }
        }
    }

    SatLimits limits_;
    std::size_t literals_left_;
    WorkBudget work_;
    Literal variables_ = 0, truth_ = 0;
    std::map<std::string, Literal, std::less<>> symbols_;
    std::vector<std::string> symbol_order_;
    std::vector<Clause> clauses_;
};
} // namespace sat_detail

/// @implstring Bounded deterministic DPLL over a fresh, definitional Tseitin
/// CNF for each check. Takes normalized Core Boolean terms with valid token
/// spellings and canonical variable names (without quoting delimiters).
/// Core Boolean operators and constants cannot be declared as variables.
/// Unsupported terms or exhausted bounds give unknown. SAT models are checked
/// against the original terms. No retained search state, theory solving, or
/// proof output.
class BoolSolver {
public:
    static constexpr std::size_t default_max_variables = sat_detail::default_max_variables;
    explicit BoolSolver(std::size_t max_variables = default_max_variables, SatLimits limits = {})
        : max_variables_(max_variables), limits_(limits) {}

    static bool evaluate(const SExpr& e, const BoolModel& model, bool& result) {
        return sat_detail::evaluate(e, model, result, sat_detail::safe_depth);
    }

    SolveResult check(const std::vector<std::string>& variables, const std::vector<SExpr>& assertions) const {
        if (variables.size() > max_variables_) return {};
        sat_detail::Cnf cnf(limits_);
        for (const auto& name : variables)
            if (!cnf.declare_symbol(name, max_variables_)) return {};
        std::vector<sat_detail::Literal> roots;
        std::vector<const SExpr*> originals;
        for (const auto& a : assertions) {
            const auto root = cnf.encode_root(a);
            if (!root) return {};
            roots.push_back(root);
            originals.push_back(&a);
        }
        return cnf.solve(roots, originals);
    }

private:
    std::size_t max_variables_;
    SatLimits limits_;
};

/// Truth-table reference for small differential tests.
class ReferenceBoolSolver {
public:
    explicit ReferenceBoolSolver(std::size_t max_variables = 16) : max_variables_(max_variables) {}

    SolveResult check(const std::vector<std::string>& variables, const std::vector<SExpr>& assertions) const {
        if (variables.size() > max_variables_ || variables.size() >= 63) return {};
        BoolModel model;
        std::vector<BoolModel::iterator> bindings;
        for (const auto& v : variables) {
            if (sat_detail::reserved(v)) return {};
            const auto entry = model.emplace(v, false);
            if (entry.second) bindings.push_back(entry.first);
        }
        for (const auto& a : assertions) {
            bool value;
            if (!BoolSolver::evaluate(a, model, value)) return {};
        }
        const auto count = std::uint64_t{1} << bindings.size();
        for (std::uint64_t bits = 0; bits < count; ++bits) {
            for (std::size_t i = 0; i < bindings.size(); ++i) bindings[i]->second = ((bits >> i) & 1U) != 0;
            bool all = true;
            for (const auto& a : assertions) {
                bool value;
                if (!BoolSolver::evaluate(a, model, value)) return {};
                if (!value) { all = false; break; }
            }
            if (all) return {CheckResult::sat, std::move(model)};
        }
        return {CheckResult::unsat, {}};
    }

private:
    std::size_t max_variables_;
};
} // namespace smtlib2
