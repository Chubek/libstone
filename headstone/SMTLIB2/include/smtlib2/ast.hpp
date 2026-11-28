#pragma once
#include "sexpr.hpp"
#include <optional>
#include <utility>
#include <variant>

namespace smtlib2 {

struct SymbolSyntax {
    std::string spelling;
    Span span;
    bool quoted;
    std::string name() const {
        return quoted ? spelling.substr(1, spelling.size() - 2) : spelling;
    }
};
struct NumeralSyntax { std::string spelling; Span span; };
struct ConstantSyntax { TokenKind kind; std::string spelling; Span span; };
using IndexSyntax = std::variant<NumeralSyntax, SymbolSyntax>;
struct IdentifierSyntax { SymbolSyntax symbol; std::vector<IndexSyntax> indices; Span span; };
struct SortSyntaxId { std::size_t index; };
struct TermSyntaxId { std::size_t index; };
struct QualifiedIdentifierSyntax {
    IdentifierSyntax identifier;
    std::optional<SortSyntaxId> sort;
    Span span;
};
struct SortNodeSyntax {
    IdentifierSyntax identifier;
    std::vector<SortSyntaxId> arguments;
    Span span;
};
struct AttributeSyntax {
    std::string keyword;
    std::optional<SExpr> value;
    Span span;
};
struct SortedVariableSyntax { SymbolSyntax symbol; SortSyntaxId sort; Span span; };
struct BindingSyntax { SymbolSyntax symbol; TermSyntaxId value; Span span; };
struct LetSyntax { std::vector<BindingSyntax> bindings; TermSyntaxId body; };
struct QuantifierSyntax {
    enum class Kind { forall, exists };
    Kind kind;
    std::vector<SortedVariableSyntax> variables;
    TermSyntaxId body;
};
struct ApplicationSyntax {
    QualifiedIdentifierSyntax function;
    std::vector<TermSyntaxId> arguments;
};
struct AnnotationSyntax { TermSyntaxId term; std::vector<AttributeSyntax> attributes; };
struct PatternSyntax {
    SymbolSyntax symbol;
    std::vector<SymbolSyntax> variables;
    Span span;
};
struct MatchCaseSyntax { PatternSyntax pattern; TermSyntaxId term; Span span; };
struct MatchSyntax { TermSyntaxId term; std::vector<MatchCaseSyntax> cases; };
struct TermNodeSyntax {
    using Value = std::variant<ConstantSyntax, QualifiedIdentifierSyntax, ApplicationSyntax,
                               LetSyntax, QuantifierSyntax, AnnotationSyntax, MatchSyntax>;
    Value value;
    Span span;
};

/// IDs are local to their owning syntax arena, and survive copying or moving
/// that arena. Parsing builds syntax only, not a resolved symbol environment.
struct SyntaxArena {
    std::vector<SortNodeSyntax> sorts;
    std::vector<TermNodeSyntax> terms;
    const SortNodeSyntax& operator[](SortSyntaxId id) const { return sorts.at(id.index); }
    const TermNodeSyntax& operator[](TermSyntaxId id) const { return terms.at(id.index); }
};
struct SortSyntax { SExpr syntax; SortSyntaxId root; SyntaxArena arena; };
struct TermSyntax { SExpr syntax; TermSyntaxId root; SyntaxArena arena; };

struct KeywordSyntax { std::string spelling; Span span; };
struct PropositionSyntax { SymbolSyntax symbol; bool negated; Span span; };
struct SortDeclarationSyntax { SymbolSyntax symbol; NumeralSyntax arity; Span span; };
struct SortDefinitionSyntax { SymbolSyntax symbol; std::vector<SymbolSyntax> parameters; SortSyntaxId sort; };
struct FunctionDeclarationSyntax {
    SymbolSyntax symbol;
    std::vector<SortSyntaxId> parameters;
    SortSyntaxId result;
};
struct FunctionSignatureSyntax {
    SymbolSyntax symbol;
    std::vector<SortedVariableSyntax> parameters;
    SortSyntaxId result;
    Span span;
};
struct FunctionDefinitionSyntax { FunctionSignatureSyntax signature; TermSyntaxId body; };
struct RecursiveDefinitionsSyntax {
    std::vector<FunctionSignatureSyntax> signatures;
    std::vector<TermSyntaxId> bodies;
};
struct ConstructorSyntax { SymbolSyntax symbol; std::vector<SortedVariableSyntax> selectors; Span span; };
struct DatatypeSyntax {
    std::vector<SymbolSyntax> parameters;
    std::vector<ConstructorSyntax> constructors;
    Span span;
};
struct DatatypeDeclarationSyntax { SymbolSyntax symbol; DatatypeSyntax datatype; };
struct DatatypeDeclarationsSyntax {
    std::vector<SortDeclarationSyntax> sorts;
    std::vector<DatatypeSyntax> datatypes;
};
struct CommandSyntax {
    using Value = std::variant<std::monostate, SymbolSyntax, NumeralSyntax, ConstantSyntax,
                               KeywordSyntax, AttributeSyntax, TermSyntaxId,
                               std::vector<TermSyntaxId>, std::vector<PropositionSyntax>,
                               SortDeclarationSyntax, SortDefinitionSyntax, FunctionDeclarationSyntax,
                               FunctionDefinitionSyntax, RecursiveDefinitionsSyntax,
                               DatatypeDeclarationSyntax, DatatypeDeclarationsSyntax>;
    SExpr syntax;
    CommandKind kind;
    Value value;
    SyntaxArena arena;
};
struct ScriptSyntax { std::vector<CommandSyntax> commands; };

namespace ast_detail {

inline Error syntax_error(Span span, std::string message) {
    return Error(span, std::move(message), ErrorCategory::syntax);
}
inline bool token(const SExpr& e, TokenKind kind) { return !e.is_list && e.kind == kind; }
inline bool word(const SExpr& e, const char* spelling) {
    return (token(e, TokenKind::atom) || token(e, TokenKind::reserved)) && e.atom == spelling;
}
inline bool is_symbol(const SExpr& e) {
    return token(e, TokenKind::atom) || token(e, TokenKind::quoted_symbol);
}
inline bool is_constant(const SExpr& e) {
    if (e.is_list) return false;
    switch (e.kind) {
        case TokenKind::numeral: case TokenKind::decimal: case TokenKind::binary:
        case TokenKind::hexadecimal: case TokenKind::string_literal: return true;
        default: return false;
    }
}
inline SymbolSyntax symbol(const SExpr& e) {
    if (!is_symbol(e)) throw syntax_error(e.span, "expected symbol");
    return {e.atom, e.span, e.kind == TokenKind::quoted_symbol};
}
inline NumeralSyntax numeral(const SExpr& e) {
    if (!token(e, TokenKind::numeral)) throw syntax_error(e.span, "expected numeral");
    return {e.atom, e.span};
}
inline KeywordSyntax keyword(const SExpr& e) {
    if (!token(e, TokenKind::keyword)) throw syntax_error(e.span, "expected keyword");
    return {e.atom, e.span};
}
inline const std::vector<SExpr>& list(const SExpr& e, std::size_t minimum = 0) {
    if (!e.is_list || e.children.size() < minimum)
        throw syntax_error(e.span, minimum ? "expected list of at least " + std::to_string(minimum) + " elements"
                                           : "expected list");
    return e.children;
}
inline const std::vector<SExpr>& tuple(const SExpr& e, std::size_t n) {
    if (!e.is_list || e.children.size() != n)
        throw syntax_error(e.span, "expected list of " + std::to_string(n) + " elements");
    return e.children;
}
inline std::vector<SymbolSyntax> symbols(const SExpr& e, std::size_t minimum = 0) {
    std::vector<SymbolSyntax> out;
    for (const auto& x : list(e, minimum)) out.push_back(symbol(x));
    return out;
}
inline void check_attribute_sexpr(const SExpr& e, bool nested = false) {
    // Reserved words and keywords are S-expressions inside the list form of
    // <attribute_value>, but are not standalone attribute values.
    if (e.is_list) {
        for (const auto& child : e.children) check_attribute_sexpr(child, true);
    } else if (!is_symbol(e) && !is_constant(e)
               && !(nested && (token(e, TokenKind::keyword) || token(e, TokenKind::reserved)))) {
        throw syntax_error(e.span, nested ? "invalid token in attribute S-expression"
                                         : "expected symbol, constant, or list as attribute value");
    }
}
inline AttributeSyntax attribute(const std::vector<SExpr>& xs, std::size_t& i) {
    auto key = keyword(xs[i++]);
    AttributeSyntax result{key.spelling, std::nullopt, key.span};
    if (i < xs.size() && !token(xs[i], TokenKind::keyword)) {
        check_attribute_sexpr(xs[i]);
        result.span.end = xs[i].span.end;
        result.value = xs[i++];
    }
    return result;
}
inline bool headed(const SExpr& e, const char* spelling) {
    return e.is_list && !e.children.empty() && word(e.children.front(), spelling);
}

class Builder {
public:
    SyntaxArena arena;

    IdentifierSyntax identifier(const SExpr& e) {
        if (!e.is_list) return {symbol(e), {}, e.span};
        const auto& xs = list(e, 3);
        if (!word(xs[0], "_")) throw syntax_error(xs[0].span, "expected '_' in indexed identifier");
        IdentifierSyntax out{symbol(xs[1]), {}, e.span};
        for (std::size_t i = 2; i < xs.size(); ++i) {
            if (token(xs[i], TokenKind::numeral)) out.indices.emplace_back(numeral(xs[i]));
            else out.indices.emplace_back(symbol(xs[i]));
        }
        return out;
    }

    SortSyntaxId sort(const SExpr& e) {
        SortNodeSyntax out = [&]() -> SortNodeSyntax {
            if (!e.is_list || headed(e, "_")) return {identifier(e), {}, e.span};
            const auto& xs = list(e, 2);
            SortNodeSyntax s{identifier(xs[0]), {}, e.span};
            for (std::size_t i = 1; i < xs.size(); ++i) s.arguments.push_back(sort(xs[i]));
            return s;
        }();
        const SortSyntaxId id{arena.sorts.size()};
        arena.sorts.push_back(std::move(out));
        return id;
    }

    QualifiedIdentifierSyntax qualified(const SExpr& e) {
        if (!headed(e, "as")) return {identifier(e), std::nullopt, e.span};
        const auto& xs = tuple(e, 3);
        return {identifier(xs[1]), sort(xs[2]), e.span};
    }

    SortedVariableSyntax sorted_variable(const SExpr& e) {
        const auto& xs = tuple(e, 2);
        return {symbol(xs[0]), sort(xs[1]), e.span};
    }

    std::vector<SortedVariableSyntax> sorted_variables(const SExpr& e, std::size_t minimum = 0) {
        std::vector<SortedVariableSyntax> out;
        for (const auto& x : list(e, minimum)) out.push_back(sorted_variable(x));
        return out;
    }

    TermSyntaxId term(const SExpr& e) {
        auto value = term_value(e);
        const TermSyntaxId id{arena.terms.size()};
        arena.terms.push_back({std::move(value), e.span});
        return id;
    }

    std::vector<TermSyntaxId> terms(const SExpr& e, std::size_t minimum = 0) {
        std::vector<TermSyntaxId> out;
        for (const auto& x : list(e, minimum)) out.push_back(term(x));
        return out;
    }

    std::vector<SortSyntaxId> sorts(const SExpr& e) {
        std::vector<SortSyntaxId> out;
        for (const auto& x : list(e)) out.push_back(sort(x));
        return out;
    }

    DatatypeSyntax datatype(const SExpr& e) {
        DatatypeSyntax out{{}, {}, e.span};
        if (headed(e, "par")) {
            const auto& xs = tuple(e, 3);
            out.parameters = symbols(xs[1], 1);
            out.constructors = constructors(xs[2]);
        } else {
            out.constructors = constructors(e);
        }
        return out;
    }

private:
    static PatternSyntax pattern(const SExpr& e) {
        if (!e.is_list) return {symbol(e), {}, e.span};
        const auto& xs = list(e, 2);
        PatternSyntax out{symbol(xs.front()), {}, e.span};
        for (std::size_t i = 1; i < xs.size(); ++i) out.variables.push_back(symbol(xs[i]));
        return out;
    }

    TermNodeSyntax::Value term_value(const SExpr& e) {
        if (is_constant(e)) return ConstantSyntax{e.kind, e.atom, e.span};
        if (!e.is_list || headed(e, "_") || headed(e, "as")) return qualified(e);
        const auto& xs = list(e, 2);
        if (word(xs[0], "let")) {
            tuple(e, 3);
            std::vector<BindingSyntax> bindings;
            // RHSs and body stay separate: an elaborator must resolve all RHSs
            // in the outer scope before introducing any of these bindings.
            for (const auto& x : list(xs[1], 1)) {
                const auto& pair = tuple(x, 2);
                bindings.push_back({symbol(pair[0]), term(pair[1]), x.span});
            }
            return LetSyntax{std::move(bindings), term(xs[2])};
        }
        if (word(xs[0], "forall") || word(xs[0], "exists")) {
            tuple(e, 3);
            return QuantifierSyntax{
                word(xs[0], "forall") ? QuantifierSyntax::Kind::forall : QuantifierSyntax::Kind::exists,
                sorted_variables(xs[1], 1), term(xs[2])};
        }
        if (word(xs[0], "!")) {
            list(e, 3);
            AnnotationSyntax out{term(xs[1]), {}};
            for (std::size_t i = 2; i < xs.size();) out.attributes.push_back(attribute(xs, i));
            return out;
        }
        if (word(xs[0], "match")) {
            tuple(e, 3);
            MatchSyntax out{term(xs[1]), {}};
            for (const auto& x : list(xs[2], 1)) {
                const auto& pair = tuple(x, 2);
                out.cases.push_back({pattern(pair[0]), term(pair[1]), x.span});
            }
            return out;
        }
        ApplicationSyntax out{qualified(xs[0]), {}};
        for (std::size_t i = 1; i < xs.size(); ++i) out.arguments.push_back(term(xs[i]));
        return out;
    }

    std::vector<ConstructorSyntax> constructors(const SExpr& e) {
        std::vector<ConstructorSyntax> out;
        for (const auto& x : list(e, 1)) {
            const auto& xs = list(x, 1);
            ConstructorSyntax ctor{symbol(xs.front()), {}, x.span};
            for (std::size_t i = 1; i < xs.size(); ++i) ctor.selectors.push_back(sorted_variable(xs[i]));
            out.push_back(std::move(ctor));
        }
        return out;
    }
};

inline SortDeclarationSyntax sort_declaration(const SExpr& e) {
    const auto& xs = tuple(e, 2);
    return {symbol(xs[0]), numeral(xs[1]), e.span};
}

inline std::vector<PropositionSyntax> propositions(const SExpr& e) {
    std::vector<PropositionSyntax> out;
    for (const auto& x : list(e)) {
        if (!x.is_list) {
            out.push_back({symbol(x), false, x.span});
        } else {
            const auto& xs = tuple(x, 2);
            if (!word(xs[0], "not")) throw syntax_error(xs[0].span, "expected 'not' in propositional literal");
            out.push_back({symbol(xs[1]), true, x.span});
        }
    }
    return out;
}

} // namespace ast_detail

inline SortSyntax make_sort(SExpr e) {
    ast_detail::Builder builder;
    const auto id = builder.sort(e);
    return {std::move(e), id, std::move(builder.arena)};
}

inline TermSyntax make_term(SExpr e) {
    ast_detail::Builder builder;
    const auto id = builder.term(e);
    return {std::move(e), id, std::move(builder.arena)};
}

inline CommandSyntax make_command(SExpr e) {
    using namespace ast_detail;
    using K = CommandKind;
    const auto& xs = list(e, 1);
    // Accept a bare atom here too: callers can build concrete SExprs directly.
    if (!token(xs.front(), TokenKind::reserved) && !token(xs.front(), TokenKind::atom))
        throw syntax_error(xs.front().span, "expected command name");
    const auto& name = xs.front().atom;
    const auto* command = detail::find_command(name);
    if (!command)
        throw syntax_error(xs.front().span, "unrecognized SMT-LIB 2.6 command: " + name);
    const auto kind = command->kind;
    if ((kind == K::set_info || kind == K::set_option) ? (xs.size() < 2 || xs.size() > 3)
                                                    : xs.size() != command->arity + 1)
        throw syntax_error(e.span, "wrong command arity: " + name);

    Builder builder;
    CommandSyntax::Value value;
    switch (kind) {
        case K::assert_: value = builder.term(xs[1]); break;
        case K::check_sat_assuming: value = propositions(xs[1]); break;
        case K::declare_const:
            value = FunctionDeclarationSyntax{symbol(xs[1]), {}, builder.sort(xs[2])};
            break;
        case K::declare_datatype:
            value = DatatypeDeclarationSyntax{symbol(xs[1]), builder.datatype(xs[2])};
            break;
        case K::declare_datatypes: {
            const auto& sorts = list(xs[1], 1);
            const auto& datatypes = list(xs[2], 1);
            if (sorts.size() != datatypes.size())
                throw syntax_error(xs[2].span, "expected one datatype declaration per sort declaration");
            DatatypeDeclarationsSyntax declarations;
            for (const auto& x : sorts) declarations.sorts.push_back(sort_declaration(x));
            for (const auto& x : datatypes) declarations.datatypes.push_back(builder.datatype(x));
            value = std::move(declarations);
            break;
        }
        case K::declare_fun:
            value = FunctionDeclarationSyntax{symbol(xs[1]), builder.sorts(xs[2]), builder.sort(xs[3])};
            break;
        case K::declare_sort:
            value = SortDeclarationSyntax{symbol(xs[1]), numeral(xs[2]), {xs[1].span.begin, xs[2].span.end}};
            break;
        case K::define_fun: case K::define_fun_rec:
            value = FunctionDefinitionSyntax{
                {symbol(xs[1]), builder.sorted_variables(xs[2]), builder.sort(xs[3]), {xs[1].span.begin, xs[3].span.end}},
                builder.term(xs[4])};
            break;
        case K::define_funs_rec: {
            const auto& signatures = list(xs[1], 1);
            if (signatures.size() != list(xs[2], 1).size())
                throw syntax_error(xs[2].span, "expected one body per recursive function signature");
            RecursiveDefinitionsSyntax definitions;
            for (const auto& x : signatures) {
                const auto& signature = tuple(x, 3);
                definitions.signatures.push_back(
                    {symbol(signature[0]), builder.sorted_variables(signature[1]), builder.sort(signature[2]), x.span});
            }
            definitions.bodies = builder.terms(xs[2], 1);
            value = std::move(definitions);
            break;
        }
        case K::define_sort:
            value = SortDefinitionSyntax{symbol(xs[1]), symbols(xs[2]), builder.sort(xs[3])};
            break;
        case K::echo:
            if (!token(xs[1], TokenKind::string_literal)) throw syntax_error(xs[1].span, "expected string literal");
            value = ConstantSyntax{xs[1].kind, xs[1].atom, xs[1].span};
            break;
        case K::get_info: case K::get_option: value = keyword(xs[1]); break;
        case K::get_value: value = builder.terms(xs[1], 1); break;
        case K::pop: case K::push: value = numeral(xs[1]); break;
        case K::set_info: case K::set_option: {
            std::size_t i = 1;
            value = attribute(xs, i);
            if (i != xs.size()) throw syntax_error(xs[i].span, "unexpected keyword as attribute value");
            break;
        }
        case K::set_logic: value = symbol(xs[1]); break;
        default: break;
    }
    return {std::move(e), kind, std::move(value), std::move(builder.arena)};
}

inline ScriptSyntax make_script(std::vector<SExpr> expressions) {
    ScriptSyntax result;
    result.commands.reserve(expressions.size());
    for (auto& expression : expressions) result.commands.push_back(make_command(std::move(expression)));
    return result;
}

/// @implstring Parse SMT-LIB 2.6 command and term shapes, preserving concrete
/// spelling and source ranges. This does not resolve names, check sorts, or
/// validate the values of individual options and attributes.
inline ScriptSyntax parse_script(std::string_view source, Limits limits = {}) {
    return make_script(parse(source, limits));
}

/// Render the owned concrete syntax with normalized whitespace. Comments are
/// not retained, and these overloads do not serialize edits to arena nodes.
inline std::string print(const SortSyntax& sort) { return print(sort.syntax); }
inline std::string print(const TermSyntax& term) { return print(term.syntax); }
inline std::string print(const CommandSyntax& command) { return print(command.syntax); }
inline std::string print(const ScriptSyntax& script) {
    std::string out;
    for (const auto& command : script.commands) {
        if (!out.empty()) out += '\n';
        out += print(command);
    }
    return out;
}

} // namespace smtlib2
