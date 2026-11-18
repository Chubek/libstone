#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace firestone::lr {

using Symbol = std::size_t;

struct GrammarSymbol {
    std::string name;
    bool terminal;
};

struct Production {
    Symbol lhs;
    std::vector<Symbol> rhs;
};

class Grammar {
public:
    const std::vector<GrammarSymbol>& symbols() const { return symbols_; }
    const std::vector<Production>& productions() const { return productions_; }
    Symbol start() const { return start_; }
    Symbol eof() const { return 0; }

    // Terminal and nonterminal names are interned in separate namespaces.
    std::optional<Symbol> findTerminal(std::string_view name) const { return find(name, true); }
    std::optional<Symbol> findNonterminal(std::string_view name) const { return find(name, false); }
    Symbol terminal(std::string name) { return intern(std::move(name), true); }
    Symbol nonterminal(std::string name) { return intern(std::move(name), false); }

    void setStart(Symbol symbol) {
        requireNonterminal(symbol);
        start_ = symbol;
    }

    // Returns the production ID. Duplicate rules are distinct productions.
    std::size_t addProduction(Symbol lhs, std::vector<Symbol> rhs) {
        requireNonterminal(lhs);
        validateRhs(rhs);
        productions_.push_back({lhs, std::move(rhs)});
        return productions_.size() - 1;
    }

    void validate() const {
        requireNonterminal(start_);
        std::vector<bool> defined(symbols_.size());
        for (const auto& production : productions_) {
            requireNonterminal(production.lhs);
            validateRhs(production.rhs);
            defined[production.lhs] = true;
        }
        for (Symbol symbol = 0; symbol < symbols_.size(); ++symbol) {
            if (!symbols_[symbol].terminal && !defined[symbol]) {
                throw std::invalid_argument("no productions for nonterminal: " + symbols_[symbol].name);
            }
        }
        // Unreachable and unproductive rules are valid CFG rules.
    }

private:
    static constexpr std::string_view eofName_ = "$end";
    std::vector<GrammarSymbol> symbols_{{std::string(eofName_), true}};
    std::vector<Production> productions_;
    Symbol start_ = std::numeric_limits<Symbol>::max();

    std::optional<Symbol> find(std::string_view name, bool terminalSymbol) const {
        for (Symbol symbol = 0; symbol < symbols_.size(); ++symbol) {
            if (symbols_[symbol].terminal == terminalSymbol && symbols_[symbol].name == name) return symbol;
        }
        return std::nullopt;
    }

    Symbol intern(std::string name, bool terminalSymbol) {
        if (name.empty() || name == eofName_) throw std::invalid_argument("empty or reserved symbol name");
        if (const auto symbol = find(name, terminalSymbol)) return *symbol;
        symbols_.push_back({std::move(name), terminalSymbol});
        return symbols_.size() - 1;
    }

    void requireNonterminal(Symbol symbol) const {
        if (symbol >= symbols_.size() || symbols_[symbol].terminal) {
            throw std::invalid_argument("expected a nonterminal symbol");
        }
    }

    void validateRhs(const std::vector<Symbol>& rhs) const {
        for (Symbol symbol : rhs) {
            if (symbol >= symbols_.size() || symbol == eof()) {
                throw std::invalid_argument("invalid production RHS symbol");
            }
        }
    }
};

struct ParseNode {
    Symbol symbol;
    std::size_t begin, end; // Half-open interval in the public input token vector.
    std::vector<ParseNode> children;
};

inline void validateInput(const Grammar& grammar, const std::vector<Symbol>& input) {
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto symbol = input[i];
        if (symbol >= grammar.symbols().size() || !grammar.symbols()[symbol].terminal ||
            symbol == grammar.eof()) {
            throw std::invalid_argument("invalid input terminal at token " + std::to_string(i));
        }
    }
}

namespace detail {

class EBNFReader {
public:
    explicit EBNFReader(std::string_view source) : source_(source) {}

    Grammar read() {
        std::optional<Symbol> first, start;
        while (peek() != '\0') {
            if (take('%')) {
                if (identifier() != "start" || start) fail("expected a single %start declaration");
                start = grammar_.nonterminal(identifier());
                require(';');
                continue;
            }
            const auto lhs = grammar_.nonterminal(identifier());
            if (!first) first = lhs;
            if (take(':')) require(':');
            require('=');
            add(lhs, expression(0));
            require(';');
        }
        if (!first) fail("expected a production");
        grammar_.setStart(start.value_or(*first));
        grammar_.validate();
        return std::move(grammar_);
    }

private:
    using Alternatives = std::vector<std::vector<Symbol>>;
    std::string_view source_;
    Grammar grammar_;
    std::size_t offset_ = 0, helpers_ = 0;

    static bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool digit(char c) { return c >= '0' && c <= '9'; }

    char peek() {
        while (offset_ < source_.size()) {
            const auto c = source_[offset_];
            if (c == '#') {
                while (offset_ < source_.size() && source_[offset_] != '\n') ++offset_;
            } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v') {
                ++offset_;
            } else {
                if (c == '\0') fail("unexpected NUL");
                return c;
            }
        }
        return '\0';
    }

    bool take(char c) {
        if (peek() != c) return false;
        ++offset_;
        return true;
    }

    void require(char c) {
        if (!take(c)) fail(std::string("expected '") + c + "'");
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw std::invalid_argument("EBNF byte " + std::to_string(offset_) + ": " + message);
    }

    std::string identifier() {
        if (!letter(peek())) fail("expected a nonterminal name");
        const auto begin = offset_++;
        while (offset_ < source_.size() && (letter(source_[offset_]) || digit(source_[offset_]))) ++offset_;
        return std::string(source_.substr(begin, offset_ - begin));
    }

    Symbol literal() {
        const auto quote = source_[offset_++];
        std::string text;
        while (offset_ < source_.size()) {
            auto c = source_[offset_++];
            if (c == quote) {
                if (text.empty() || text == grammar_.symbols()[grammar_.eof()].name) {
                    fail("empty or reserved terminal literal; use an empty alternative for epsilon");
                }
                return grammar_.terminal(std::move(text));
            }
            if (c == '\\') {
                if (offset_ == source_.size()) fail("unterminated escape");
                c = source_[offset_++];
                if (c == 'n') c = '\n';
                else if (c == 'r') c = '\r';
                else if (c == 't') c = '\t';
                else if (c != '\'' && c != '"' && c != '\\') fail("unknown escape");
            } else if (c == '\0' || c == '\n' || c == '\r') {
                fail("unexpected NUL or line break in terminal literal");
            }
            text.push_back(c);
        }
        fail("unterminated terminal literal");
    }

    void add(Symbol lhs, Alternatives alternatives) {
        for (auto& rhs : alternatives) grammar_.addProduction(lhs, std::move(rhs));
    }

    Symbol helperSymbol() { return grammar_.nonterminal("@ebnf" + std::to_string(helpers_++)); }

    Symbol lower(Alternatives alternatives, char modifier) {
        const auto helper = helperSymbol();
        const auto tail = modifier == '+' ? helperSymbol() : helper;
        for (auto& rhs : alternatives) {
            if (modifier == '*' || modifier == '+') rhs.push_back(tail);
            if (modifier == '+') grammar_.addProduction(helper, rhs);
        }
        add(tail, std::move(alternatives));
        // Ordered-choice consumers can try the body before the generated
        // empty fallback. Explicit alternatives retain their source order.
        if (modifier == '?' || modifier == '*' || modifier == '+') grammar_.addProduction(tail, {});
        return helper;
    }

    Symbol atom(std::size_t depth) {
        const auto c = peek();
        Symbol symbol;
        if (letter(c)) {
            symbol = grammar_.nonterminal(identifier());
        } else if (c == '\'' || c == '"') {
            symbol = literal();
        } else if (c == '(' || c == '[' || c == '{') {
            ++offset_;
            auto alternatives = expression(depth + 1);
            require(c == '(' ? ')' : c == '[' ? ']' : '}');
            symbol = lower(std::move(alternatives), c == '[' ? '?' : c == '{' ? '*' : '\0');
        } else {
            fail("expected a nonterminal, terminal literal, or group");
        }
        const auto modifier = peek();
        if (modifier == '?' || modifier == '*' || modifier == '+') {
            ++offset_;
            symbol = lower({{symbol}}, modifier);
        }
        return symbol;
    }

    Alternatives expression(std::size_t depth) {
        if (depth > 256) fail("group nesting exceeds 256");
        Alternatives result;
        do {
            std::vector<Symbol> sequence;
            for (auto c = peek(); c != '|' && c != ';' && c != ')' && c != ']' && c != '}' && c != '\0';
                 c = peek()) {
                sequence.push_back(atom(depth));
            }
            result.push_back(std::move(sequence));
        } while (take('|'));
        return result;
    }
};

} // namespace detail

// Rules use `name = expression;` or `name ::= expression;`. Adjacency joins
// atoms, | separates alternatives, and an empty alternative means epsilon.
// Atoms are bare nonterminal names, single/double-quoted token names, or
// groups (), [], {} (group, optional, repetition); postfix ?, *, + also work.
// # starts a line comment. %start name; overrides the default first rule.
// Desugaring introduces ordinary nonterminals, visible in parser results.
inline Grammar readEBNF(std::string_view source) {
    return detail::EBNFReader(source).read();
}

} // namespace firestone::lr
