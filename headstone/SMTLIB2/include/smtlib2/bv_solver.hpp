#pragma once
#include "elaboration.hpp"
#include "solver.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smtlib2::bv_solver {
/// @implstring Finite exhaustive solver for nullary Bool (width 0) and BV
/// (width 1..64) symbols. Binary and hex literals; unary/binary operations
/// listed below. No implicit coercions, indexed operations, or definitions.
/// Search enumerates at most 16 bits of referenced symbols. Declarations,
/// compiled nodes, candidate bindings, evaluation instructions, and source
/// nodes visited during model validation share a structural work budget.
using Environment = std::map<std::string, unsigned>;
using Model = std::map<std::string, std::uint64_t>;
struct Result {
    CheckResult status;
    Model model;
    std::string reason;
};

namespace detail {
enum class Op { constant, variable, neg, inv, add, sub, mul, band, bor, bxor,
                ult, eq, ne, lnot, land, lor, lxor, implies };
struct Operator { std::string_view name; Op op; unsigned arity; bool bv; };
inline constexpr Operator operators[] = {
    {"bvneg", Op::neg, 1, true}, {"bvnot", Op::inv, 1, true},
    {"bvadd", Op::add, 2, true}, {"bvsub", Op::sub, 2, true},
    {"bvmul", Op::mul, 2, true}, {"bvand", Op::band, 2, true},
    {"bvor", Op::bor, 2, true}, {"bvxor", Op::bxor, 2, true},
    {"bvult", Op::ult, 2, true}, {"=", Op::eq, 2, false},
    {"distinct", Op::ne, 2, false}, {"not", Op::lnot, 1, false},
    {"and", Op::land, 2, false}, {"or", Op::lor, 2, false},
    {"xor", Op::lxor, 2, false}, {"=>", Op::implies, 2, false}
};
inline const Operator* lookup(std::string_view name) {
    for (const auto& op : operators) if (op.name == name) return &op;
    return nullptr;
}
inline std::uint64_t mask(unsigned w) {
    if (w > 64) throw std::invalid_argument("unsupported BV width");
    return !w ? 1 : w == 64 ? UINT64_MAX : (std::uint64_t{1} << w) - 1;
}
struct Instruction {
    Op op;
    unsigned arity, width;
    std::uint64_t data;
    std::string name;
};
using Program = std::vector<Instruction>;

inline Instruction leaf(const SExpr& e, const Environment& env) {
    if (!e.children.empty()) throw Error(e.span, "BV atom has children", ErrorCategory::syntax);
    unsigned w = 0;
    std::uint64_t n = 0;
    if (e.kind == TokenKind::binary || e.kind == TokenKind::hexadecimal) {
        const unsigned bits = e.kind == TokenKind::binary ? 1 : 4;
        if (e.atom.size() < 3 || e.atom.size() - 2 > 64 / bits ||
            e.atom[0] != '#' || e.atom[1] != (bits == 1 ? 'b' : 'x'))
            throw Error(e.span, "expected BV literal of width 1..64");
        for (std::size_t i = 2; i < e.atom.size(); ++i) {
            const auto c = e.atom[i];
            const unsigned d = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (d >= (1u << bits)) throw Error(e.span, "invalid BV literal digit");
            n = (n << bits) | d;
            w += bits;
        }
    } else {
        auto key = bool_symbol(e);
        if (key == "true" || key == "false") n = key == "true";
        else {
            const auto it = env.find(key);
            if (it == env.end()) throw Error(e.span, "unknown BV fragment symbol: " + key);
            if (it->second > 64) throw Error(e.span, "unsupported BV width");
            return {Op::variable, 0, it->second, 0, std::move(key)};
        }
    }
    return {Op::constant, 0, w, n, {}};
}

inline bool is_comparison(Op op) noexcept {
    return op == Op::eq || op == Op::ne || op == Op::ult;
}

inline unsigned compile(const SExpr& e, const Environment& env, Program& p,
                        unsigned depth = 0, sat_detail::WorkBudget* work = nullptr) {
    if (depth > 256)
        throw Error(e.span, "BV expression limit exceeded", ErrorCategory::resource_limit);
    if (work && !work->spend())
        throw Error(e.span, "BV work limit", ErrorCategory::resource_limit);
    auto emit = [&](Instruction instruction) {
        // Check at emission: a child may have filled the program while its
        // parent's instruction was still pending.
        if (p.size() >= 65536)
            throw Error(e.span, "BV expression limit exceeded", ErrorCategory::resource_limit);
        p.push_back(std::move(instruction));
    };
    if (!e.is_list) {
        auto inst = leaf(e, env);
        const auto w = inst.width;
        emit(std::move(inst));
        return w;
    }
    if (!e.atom.empty()) throw Error(e.span, "BV application has atom payload", ErrorCategory::syntax);
    if (e.children.empty()) throw Error(e.span, "empty BV application");
    const auto& head = e.children[0];
    const auto* op = lookup(bool_elaboration::symbol_view(head, head.span));
    if (!op) throw Error(head.span, "unsupported BV fragment operator");
    if (e.children.size() != op->arity + 1) throw Error(e.span, "unsupported operator arity");
    const auto w = compile(e.children[1], env, p, depth + 1, work);
    const bool equality = op->op == Op::eq || op->op == Op::ne;
    if (!equality && (w != 0) != op->bv) throw Error(e.children[1].span, "operand sort mismatch");
    if (op->arity == 2 && compile(e.children[2], env, p, depth + 1, work) != w)
        throw Error(e.children[2].span, "operand sorts differ");
    const unsigned out = is_comparison(op->op) ? 0 : w;
    emit({op->op, op->arity, out, 0, {}});
    return out;
}

inline std::uint64_t binding(const Instruction& i, const Model& model) {
    const auto n = model.at(i.name);
    if (n > mask(i.width)) throw std::invalid_argument("out-of-sort BV model binding");
    return n;
}

inline std::uint64_t apply(Op op, unsigned width, std::uint64_t a, std::uint64_t b) {
    switch (op) {
    case Op::neg: a = -a; break;
    case Op::inv: a = ~a; break;
    case Op::add: a += b; break;
    case Op::sub: a -= b; break;
    case Op::mul: a *= b; break;
    case Op::band: case Op::land: a &= b; break;
    case Op::bor: case Op::lor: a |= b; break;
    case Op::bxor: case Op::lxor: a ^= b; break;
    case Op::ult: a = a < b; break;
    case Op::eq: a = a == b; break;
    case Op::ne: a = a != b; break;
    case Op::lnot: a = !a; break;
    case Op::implies: a = !a || b; break;
    default: throw std::logic_error("invalid BV operator");
    }
    return a & mask(width);
}

inline std::uint64_t run(const Program& p, const Model& model, std::vector<std::uint64_t>& stack) {
    stack.clear();
    for (const auto& i : p) {
        if (i.op == Op::constant) { stack.push_back(i.data); continue; }
        if (i.op == Op::variable) {
            stack.push_back(binding(i, model));
            continue;
        }
        std::uint64_t b = 0;
        if (i.arity == 2) { b = stack.back(); stack.pop_back(); }
        stack.back() = apply(i.op, i.width, stack.back(), b);
    }
    return stack.back();
}

struct TypedValue {
    unsigned width = 0;
    std::uint64_t value = 0;
};

// Walk the already checked source rather than rerunning the compiled tape:
// verify lowering, widths and operand order independently of the stack VM.
// The prior compilation also bounds the recursion depth.
inline bool evaluate_source(const SExpr& e, const Environment& env, const Model& model,
                            TypedValue& out, sat_detail::WorkBudget& work) {
    if (!work.spend()) return false;
    if (!e.is_list) {
        const auto i = leaf(e, env);
        out = {i.width, i.op == Op::variable ? binding(i, model) : i.data};
        return true;
    }
    const auto& head = e.children.front();
    const auto& op = *lookup(bool_elaboration::symbol_view(head, head.span));
    TypedValue a, b;
    if (!evaluate_source(e.children[1], env, model, a, work) ||
        (op.arity == 2 && !evaluate_source(e.children[2], env, model, b, work))) return false;
    const auto w = is_comparison(op.op) ? 0 : a.width;
    out = {w, apply(op.op, w, a.value, b.value)};
    return true;
}
} // namespace detail

inline bool builtin(std::string_view name) { return is_bool_builtin(name) || detail::lookup(name); }
inline unsigned width(const SExpr& term, const Environment& env) {
    detail::Program p;
    return detail::compile(term, env, p);
}
inline std::uint64_t value(const SExpr& term, const Environment& env, const Model& model) {
    detail::Program p;
    detail::compile(term, env, p);
    std::vector<std::uint64_t> stack;
    return detail::run(p, model, stack);
}
inline std::string literal(unsigned width, std::uint64_t n) {
    if (n > detail::mask(width)) throw std::invalid_argument("out-of-sort BV literal");
    if (!width) return n ? "true" : "false";
    std::string text = "#b";
    for (unsigned i = width; i; --i) text += ((n >> (i - 1)) & 1) ? '1' : '0';
    return text;
}
inline Result check(const Environment& env, const std::vector<SExpr>& assertions,
                    std::size_t max_bits = 16, std::size_t max_work = 10000000) {
    const auto exhausted = [] { return Result{CheckResult::unknown, {}, "BV work limit"}; };
    sat_detail::WorkBudget work{max_work};
    Model model;
    for (const auto& entry : env) {
        if (!work.spend()) return exhausted();
        if (entry.second > 64 || builtin(entry.first)) throw Error({}, "invalid BV declaration");
        model.emplace(entry.first, 0);
    }
    std::vector<detail::Program> programs;
    std::map<std::string,unsigned> referenced;
    for (const auto& term : assertions) {
        auto& p = programs.emplace_back();
        try {
            if (detail::compile(term, env, p, 0, &work))
                throw Error(term.span, "assertion must have Bool sort");
        } catch (const Error& e) {
            if (e.category() != ErrorCategory::resource_limit) throw;
            return {CheckResult::unknown, {}, e.what()};
        }
        for (const auto& i : p)
            if (i.op == detail::Op::variable) referenced.emplace(i.name, i.width);
    }
    unsigned bits = 0;
    for (const auto& v : referenced) {
        bits += v.second ? v.second : 1;
        if (bits > std::min<std::size_t>(16, max_bits))
            return {CheckResult::unknown, {}, "BV search bit limit"};
    }
    std::vector<std::uint64_t> stack;
    for (std::uint64_t row = 0; row < (std::uint64_t{1} << bits); ++row) {
        if (!work.spend(referenced.size())) return exhausted();
        auto rest = row;
        for (const auto& v : referenced) {
            model.at(v.first) = rest & detail::mask(v.second);
            rest >>= v.second ? v.second : 1;
        }
        bool matches = true;
        for (const auto& p : programs) {
            if (!work.spend(p.size())) return exhausted();
            if (!detail::run(p, model, stack)) { matches = false; break; }
        }
        if (matches) {
            for (const auto& term : assertions) {
                detail::TypedValue checked;
                if (!detail::evaluate_source(term, env, model, checked, work)) return exhausted();
                if (checked.width != 0 || checked.value != 1)
                    return {CheckResult::unknown, {}, "invalid BV witness"};
            }
            return {CheckResult::sat, std::move(model), {}};
        }
    }
    return {CheckResult::unsat, {}, {}};
}
} // namespace smtlib2::bv_solver
