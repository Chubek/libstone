#pragma once
#include "sexpr.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smtlib2 {
/// Explicit incompleteness, distinct from an invalid term.
class UnsupportedBool : public std::runtime_error {
public:
    UnsupportedBool(Span span, std::string message)
        : UnsupportedBool(span, std::move(message), ErrorCategory::unspecified) {}
    UnsupportedBool(Span span, std::string message, ErrorCategory category)
        : std::runtime_error(std::move(message)), span_(span), category_(category) {}
    const Span& span() const noexcept { return span_; }
    ErrorCategory category() const noexcept { return category_; }
private:
    Span span_;
    ErrorCategory category_;
};

namespace bool_elaboration {
inline std::string_view symbol_view(const SExpr& expr, Span span) {
    if (!expr.is_list && expr.children.empty()) {
        if (expr.kind == TokenKind::atom && !expr.atom.empty()) return expr.atom;
        if (expr.kind == TokenKind::quoted_symbol && expr.atom.size() >= 2
            && expr.atom.front() == '|' && expr.atom.back() == '|')
            return std::string_view(expr.atom).substr(1, expr.atom.size() - 2);
    }
    throw Error(span, "expected a symbol", ErrorCategory::syntax);
}
}

inline std::string bool_symbol(const SExpr& expr) {
    return std::string(bool_elaboration::symbol_view(expr, expr.span));
}

inline bool is_bool_builtin(std::string_view name) noexcept {
    return name == "true" || name == "false" || name == "not" ||
           name == "and" || name == "or" || name == "xor" || name == "=>" ||
           name == "=" || name == "distinct" || name == "ite";
}

struct BoolDefinition {
    std::vector<std::string> parameters; // canonical names
    std::optional<SExpr> body;           // template returned by normalize_bool
};
using BoolEnvironment = std::map<std::string, BoolDefinition>;

struct BoolNormalizationLimits {
    std::size_t max_nodes = 100000;
    std::size_t max_depth = 512;
    std::size_t max_symbol_bytes = 8 * 1024 * 1024;
};

namespace bool_elaboration {
// SMT-LIB 2.6 §3.6.3 allows shadowing user symbols, not theory symbols.
inline void require_bindable(std::string_view name, Span span) {
    if (is_bool_builtin(name))
        throw Error(span, "cannot bind Boolean theory symbol: " + std::string(name));
}

// Shared, immutable Bool nodes retain sharing through simultaneous let and
// parameter substitution. Only the final solver input is unfolded into a tree.
struct Node;
using Term = std::shared_ptr<const Node>;
struct Node {
    SExpr head;
    std::vector<Term> args;
    std::size_t depth;
};

struct Scope {
    const Scope* outer = nullptr;
    std::map<std::string, Term, std::less<>> bindings;
    Term find(std::string_view name) const {
        for (auto scope = this; scope; scope = scope->outer) {
            const auto it = scope->bindings.find(name);
            if (it != scope->bindings.end()) return it->second;
        }
        return {};
    }
};

class Normalizer {
public:
    Normalizer(const BoolEnvironment& env, BoolNormalizationLimits limits)
        : env_(env), limits_(limits) {
        limits_.max_depth = std::min(limits_.max_depth, std::size_t{512});
    }

    SExpr run(const SExpr& syntax, const std::vector<std::string>& parameters) {
        Scope scope;
        for (const auto& name : parameters) {
            charge_bytes(syntax.span, name.size());
            require_bindable(name, syntax.span);
            // lparen tokens cannot occur as atoms in a parsed SExpr. Use this
            // internal marker instead of inventing a spellable parameter name.
            if (!scope.bindings.emplace(name, make(TokenKind::lparen, name, syntax.span)).second)
                throw Error(syntax.span, "duplicate function parameter: " + name);
        }
        return emit(elaborate(syntax, scope, 1));
    }

private:
    using Arguments = std::map<std::string_view, Term>;

    void charge(Span span, std::size_t depth = 1) {
        if (remaining_ == 0 || depth > limits_.max_depth)
            throw UnsupportedBool(span, "Boolean normalization limit exceeded",
                                  ErrorCategory::resource_limit);
        --remaining_;
    }

    void charge_bytes(Span span, std::size_t bytes) {
        if (bytes > remaining_bytes_)
            throw UnsupportedBool(span, "Boolean normalization symbol byte limit exceeded",
                                  ErrorCategory::resource_limit);
        remaining_bytes_ -= bytes;
    }

    Term make(TokenKind kind, std::string_view spelling, Span span,
              std::vector<Term> args = {}) {
        std::size_t depth = 1;
        for (const auto& arg : args) depth = std::max(depth, arg->depth + 1);
        charge(span, depth);
        charge_bytes(span, spelling.size());
        SExpr head{false, kind, std::string(spelling), {}, span};
        return std::make_shared<const Node>(Node{std::move(head), std::move(args), depth});
    }

    static void validate_builtin(std::string_view name, std::size_t arity, Span span) {
        const bool valid = (name == "true" || name == "false") ? arity == 0
                         : name == "not" ? arity == 1
                         : name == "ite" ? arity == 3
                         : arity >= 2;
        if (!valid) throw Error(span, "wrong arity for " + std::string(name));
    }

    Term elaborate_let(const SExpr& e, const Scope& scope, std::size_t depth) {
        if (e.children.size() != 3 || !e.children[1].is_list || e.children[1].children.empty())
            throw Error(e.span, "expected (let ((symbol term) ...) term)", ErrorCategory::syntax);
        Scope nested{&scope, {}};
        for (const auto& binding : e.children[1].children) {
            charge(binding.span);
            if (!binding.is_list || binding.children.size() != 2)
                throw Error(binding.span, "expected (symbol term) binding", ErrorCategory::syntax);
            charge_bytes(binding.span, binding.children[0].atom.size());
            auto name = bool_symbol(binding.children[0]);
            require_bindable(name, binding.children[0].span);
            if (nested.bindings.count(name))
                throw Error(binding.children[0].span, "duplicate let binding: " + name);
            // All right-hand sides see only the enclosing scope.
            auto value = elaborate(binding.children[1], scope, depth + 1);
            nested.bindings.emplace(std::move(name), std::move(value));
        }
        return elaborate(e.children[2], nested, depth + 1);
    }

    Term elaborate(const SExpr& e, const Scope& scope, std::size_t depth) {
        charge(e.span, depth);
        if (e.is_list && e.children.empty())
            throw Error(e.span, "empty term application", ErrorCategory::syntax);
        const auto& head = e.is_list ? e.children.front() : e;
        if (e.is_list) {
            if (head.is_list)
                throw UnsupportedBool(head.span, "qualified and indexed term heads are unsupported");
            if (head.kind != TokenKind::quoted_symbol) {
                if (head.atom == "let") return elaborate_let(e, scope, depth);
                if (head.atom == "forall" || head.atom == "exists" || head.atom == "match" ||
                    head.atom == "!" || head.atom == "as" || head.atom == "_")
                    throw UnsupportedBool(head.span, "unsupported term form: " + head.atom);
            }
            if (e.children.size() == 1)
                throw Error(e.span, "empty term argument list", ErrorCategory::syntax);
        } else if (head.kind != TokenKind::atom && head.kind != TokenKind::quoted_symbol) {
            if (head.kind == TokenKind::string_literal || head.kind == TokenKind::numeral ||
                head.kind == TokenKind::decimal || head.kind == TokenKind::binary ||
                head.kind == TokenKind::hexadecimal)
                throw UnsupportedBool(head.span, "non-Boolean literals are unsupported");
            throw Error(head.span, "expected a Boolean term", ErrorCategory::syntax);
        }

        charge_bytes(head.span, head.atom.size());
        const auto name = symbol_view(head, head.span);
        const auto arity = e.is_list ? e.children.size() - 1 : 0;
        if (auto local = scope.find(name)) {
            if (arity != 0)
                throw Error(head.span, "local Boolean variable is not a function: " + std::string(name));
            return local;
        }
        const auto builtin = is_bool_builtin(name);
        auto definition = env_.end();
        if (builtin) {
            validate_builtin(name, arity, e.span);
        } else {
            definition = env_.find(std::string(name));
            if (definition == env_.end())
                throw Error(head.span, "undeclared Boolean symbol: " + std::string(name));
            if (arity != definition->second.parameters.size())
                throw Error(e.span, "wrong arity for " + std::string(name));
        }

        std::vector<Term> args;
        for (std::size_t i = 1; i <= arity; ++i)
            args.push_back(elaborate(e.children[i], scope, depth + 1));
        if (builtin)
            return make(TokenKind::atom, name, e.span, std::move(args));
        const auto& def = definition->second;
        if (!def.body) {
            if (arity != 0) throw UnsupportedBool(e.span, "uninterpreted Boolean functions are unsupported");
            return make(head.kind, head.atom, head.span);
        }
        Arguments parameters;
        for (std::size_t i = 0; i < arity; ++i) {
            charge_bytes(e.span, def.parameters[i].size());
            if (!parameters.emplace(def.parameters[i], std::move(args[i])).second)
                throw Error(e.span, "duplicate parameter in Boolean definition");
        }
        return substitute(*def.body, parameters, depth, e.span);
    }

    Term substitute(const SExpr& e, const Arguments& parameters, std::size_t depth, Span use) {
        // The template belongs to an earlier source. Report expansion failures
        // in this command, not at an unrelated offset in the defining input.
        charge(use, depth);
        if (!e.is_list && e.kind == TokenKind::lparen) {
            charge_bytes(use, e.atom.size());
            const auto argument = parameters.find(e.atom);
            if (!e.children.empty() || argument == parameters.end())
                throw Error(use, "invalid parameter marker in Boolean definition");
            return argument->second;
        }
        if (!e.is_list) {
            charge_bytes(use, e.atom.size());
            const auto name = symbol_view(e, use);
            if (is_bool_builtin(name)) {
                validate_builtin(name, 0, use);
            } else {
                const auto declaration = env_.find(std::string(name));
                if (declaration == env_.end() || declaration->second.body
                    || !declaration->second.parameters.empty())
                    throw Error(use, "invalid free symbol in Boolean definition: " + std::string(name));
            }
            return make(e.kind, e.atom, use);
        }
        if (e.children.size() < 2)
            throw Error(use, "invalid normalized Boolean definition");
        const auto& head = e.children.front();
        charge_bytes(use, head.atom.size());
        const auto name = symbol_view(head, use);
        if (!is_bool_builtin(name))
            throw Error(use, "invalid operator in normalized Boolean definition: " + std::string(name));
        validate_builtin(name, e.children.size() - 1, use);
        std::vector<Term> args;
        for (std::size_t i = 1; i < e.children.size(); ++i)
            args.push_back(substitute(e.children[i], parameters, depth + 1, use));
        return make(TokenKind::atom, name, use, std::move(args));
    }

    SExpr emit(const Term& term) {
        charge(term->head.span);
        charge_bytes(term->head.span, term->head.atom.size());
        if (term->args.empty()) return term->head;
        charge(term->head.span); // operator head is also an output node
        SExpr result;
        result.is_list = true;
        result.span = term->head.span;
        result.children.push_back(term->head);
        for (const auto& arg : term->args) result.children.push_back(emit(arg));
        return result;
    }

    const BoolEnvironment& env_;
    BoolNormalizationLimits limits_;
    std::size_t remaining_ = limits_.max_nodes;
    std::size_t remaining_bytes_ = limits_.max_symbol_bytes;
};
}

/// @implstring Normalize the all-Bool fragment, validating symbol and operator
/// arities and expanding nonrecursive definitions and simultaneous lets. With
/// parameters, the result is an internal function template, not solver input.
/// User symbols may be shadowed, but Core theory functions and constants cannot
/// be bound. Quantifiers, annotations, qualified/indexed terms and non-Bool
/// literals are explicitly unsupported. Syntax grammar validation belongs to
/// the AST.
inline SExpr normalize_bool(const SExpr& syntax, const BoolEnvironment& env,
                            const std::vector<std::string>& parameters = {},
                            BoolNormalizationLimits limits = {}) {
    return bool_elaboration::Normalizer(env, limits).run(syntax, parameters);
}
}
