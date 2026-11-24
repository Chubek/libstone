#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <set>
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

// Shared FIRST/nullable analysis for predictive and LR table construction.
// Epsilon is represented separately, never as an entry in a terminal set.
// The grammar must already be validated.
class GrammarFirst {
public:
    explicit GrammarFirst(const Grammar& grammar, const std::vector<bool>* includedProductions = nullptr)
        : nullable_(grammar.symbols().size()), first_(grammar.symbols().size()) {
        for (Symbol s = 0; s < first_.size(); ++s)
            if (grammar.symbols()[s].terminal) first_[s].insert(s);
        bool changed;
        do {
            changed = false;
            for (std::size_t p = 0; p < grammar.productions().size(); ++p) {
                if (includedProductions && !(*includedProductions)[p]) continue;
                const auto& rule = grammar.productions()[p];
                if (extend(first_[rule.lhs], rule.rhs, 0, changed) && !nullable_[rule.lhs]) {
                    nullable_[rule.lhs] = true;
                    changed = true;
                }
            }
        } while (changed);
    }

    bool nullable(Symbol s) const { return nullable_.at(s); }
    const std::set<Symbol>& first(Symbol s) const { return first_.at(s); }

    struct SequenceFirst {
        std::set<Symbol> terminals;
        bool nullable;
    };

    SequenceFirst sequence(const std::vector<Symbol>& rhs, std::size_t begin = 0) const {
        SequenceFirst result;
        bool changed = false;
        result.nullable = extend(result.terminals, rhs, begin, changed);
        return result;
    }

    std::vector<std::set<Symbol>> follow(
        const Grammar& grammar, const std::vector<bool>* includedProductions = nullptr) const {
        std::vector<std::set<Symbol>> result(first_.size());
        result[grammar.start()].insert(grammar.eof());
        bool changed;
        do {
            changed = false;
            for (std::size_t p = 0; p < grammar.productions().size(); ++p) {
                if (includedProductions && !(*includedProductions)[p]) continue;
                const auto& rule = grammar.productions()[p];
                auto suffix = result[rule.lhs];
                for (auto it = rule.rhs.rbegin(); it != rule.rhs.rend(); ++it) {
                    const auto s = *it;
                    if (!grammar.symbols()[s].terminal) changed |= merge(result[s], suffix);
                    if (nullable_[s]) merge(suffix, first_[s]);
                    else suffix = first_[s];
                }
            }
        } while (changed);
        return result;
    }

private:
    static bool merge(std::set<Symbol>& into, const std::set<Symbol>& from) {
        if (&into == &from) return false;
        const auto before = into.size();
        into.insert(from.begin(), from.end());
        return into.size() != before;
    }

    bool extend(std::set<Symbol>& terminals, const std::vector<Symbol>& rhs,
                std::size_t begin, bool& changed) const {
        for (auto i = begin; i < rhs.size(); ++i) {
            const auto s = rhs[i];
            changed |= merge(terminals, first_[s]);
            if (!nullable_[s]) return false;
        }
        return true;
    }

    std::vector<bool> nullable_;
    std::vector<std::set<Symbol>> first_;
};

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
                while (offset_ < source_.size() && source_[offset_] != '\n' && source_[offset_] != '\r') {
                    if (source_[offset_] == '\0') fail("unexpected NUL");
                    ++offset_;
                }
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

    unsigned char hexDigit() {
        if (offset_ == source_.size()) fail("expected two hex digits");
        const auto c = source_[offset_];
        unsigned char value;
        if (digit(c)) value = static_cast<unsigned char>(c - '0');
        else if (c >= 'a' && c <= 'f') value = static_cast<unsigned char>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value = static_cast<unsigned char>(c - 'A' + 10);
        else fail("expected two hex digits");
        ++offset_;
        return value;
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
                else if (c == '0') c = '\0';
                else if (c == 'x') {
                    const auto high = hexDigit();
                    c = static_cast<char>(16 * high + hexDigit());
                } else if (c != '\'' && c != '"' && c != '\\') fail("unknown escape");
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
// Literals accept \n, \r, \t, \0, \xHH, and escaped quotes and backslashes.
// # starts a line comment. %start name; overrides the default first rule.
// Desugaring introduces ordinary nonterminals, visible in parser results.
inline Grammar readEBNF(std::string_view source) {
    return detail::EBNFReader(source).read();
}

} // namespace firestone::lr
