#pragma once
#include "elaboration.hpp"
#include <initializer_list>
#include <memory>
#include <set>

namespace smtlib2 {
class ScriptBuilder;

/// An immutable term of sort Bool. There is no conversion from raw SMT-LIB text.
/// Terms that reference a declaration can only be asserted in its original script.
class BoolTerm {
public:
    static BoolTerm literal(bool value) { return BoolTerm(value ? "true" : "false"); }
    static BoolTerm negate(const BoolTerm& term) { return apply("not", {term}); }
    static BoolTerm conjunction(const BoolTerm& a, const BoolTerm& b) { return apply("and", {a, b}); }
    static BoolTerm disjunction(const BoolTerm& a, const BoolTerm& b) { return apply("or", {a, b}); }
    static BoolTerm exclusive_or(const BoolTerm& a, const BoolTerm& b) { return apply("xor", {a, b}); }
    static BoolTerm implies(const BoolTerm& a, const BoolTerm& b) { return apply("=>", {a, b}); }
    static BoolTerm equal(const BoolTerm& a, const BoolTerm& b) { return apply("=", {a, b}); }
    static BoolTerm ite(const BoolTerm& condition, const BoolTerm& yes, const BoolTerm& no) {
        return apply("ite", {condition, yes, no});
    }
    const std::string& text() const noexcept { return *text_; }

private:
    friend class ScriptBuilder;
    struct Identity {};
    using Owner = std::shared_ptr<const Identity>;
    explicit BoolTerm(std::string text, Owner owner = {})
        : text_(std::make_shared<const std::string>(std::move(text))), owner_(std::move(owner)) {}
    static BoolTerm apply(std::string_view op, std::initializer_list<BoolTerm> args) {
        std::string text = "(" + std::string(op);
        Owner owner;
        for (const auto& arg : args) {
            if (arg.owner_) {
                if (owner && owner != arg.owner_)
                    throw std::invalid_argument("cannot combine terms from different scripts");
                owner = arg.owner_;
            }
            if (arg.text().size() + 2 > Limits{}.max_input_bytes - text.size())
                throw std::length_error("Boolean term exceeds input size limit");
            text += ' ';
            text += arg.text();
        }
        text += ')';
        return BoolTerm(std::move(text), std::move(owner));
    }
    const std::shared_ptr<const std::string> text_;
    const Owner owner_;
};

/// @implstring Append-only Boolean script construction. Supports nullary
/// declarations and Core expressions, without a raw text escape hatch. The
/// resulting text still goes through normal parser/session limits and checks.
class ScriptBuilder {
public:
    ScriptBuilder() = default;
    ScriptBuilder(const ScriptBuilder&) = delete;
    ScriptBuilder& operator=(const ScriptBuilder&) = delete;

    // The argument is one SMT-LIB symbol spelling, e.g. "flag" or "|a flag|".
    BoolTerm declare_bool(std::string_view spelling) {
        require_open();
        const auto tokens = lex(spelling);
        if (tokens.size() != 1 || tokens.front().text.size() != spelling.size())
            throw Error({}, "expected a single Boolean symbol spelling");
        const auto& token = tokens.front();
        const auto name = bool_symbol(SExpr{false, token.kind, token.text, {}, token.span});
        if (is_bool_builtin(name) || name == "Bool")
            throw Error(token.span, "reserved Boolean symbol");
        if (names_.count(name)) throw Error(token.span, "duplicate Boolean declaration");
        BoolTerm result(token.text, owner_);
        const auto command = "(declare-const " + token.text + " Bool)\n";
        auto inserted = names_.insert(name);
        try {
            text_ += command;
        } catch (...) {
            names_.erase(inserted.first);
            throw;
        }
        return result;
    }
    void assert_term(const BoolTerm& term) {
        require_open();
        if (term.owner_ && term.owner_ != owner_)
            throw std::invalid_argument("Boolean term belongs to another script");
        text_ += "(assert " + term.text() + ")\n";
    }
    void check_sat() { append("(check-sat)\n"); }
    void exit() { append("(exit)\n"); closed_ = true; }
    const std::string& text() const noexcept { return text_; }

private:
    void require_open() const {
        if (closed_) throw std::logic_error("cannot append after exit");
    }
    void append(std::string_view command) {
        require_open();
        text_ += command;
    }
    BoolTerm::Owner owner_ = std::make_shared<const BoolTerm::Identity>();
    std::set<std::string> names_;
    std::string text_;
    bool closed_ = false;
};
}
