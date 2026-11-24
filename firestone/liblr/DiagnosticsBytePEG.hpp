#pragma once

#include "PEG.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace firestone::lr {

class LRDiagBytePEGError : public std::invalid_argument {
public:
    LRDiagBytePEGError(std::size_t offset, std::string message)
        : std::invalid_argument("byte PEG at offset " + std::to_string(offset) + ": " + message),
          offset_(offset) {}

    std::size_t offset() const noexcept { return offset_; }

private:
    std::size_t offset_;
};

// An anchored PEG over unsigned bytes, not Unicode characters. Concatenate
// atoms with optional whitespace; use / for ordered choice, (...) for groups,
// & and ! for lookahead, and ?, * and + for repetition. Atoms are quoted
// literals ('...' or "..."), byte classes ([a-z], [^a-z]), and . (any byte).
// Use !. for EOF and '' for an empty match. Literals/classes support C-style
// single byte escapes, \xHH, and escaped punctuation. Malformed patterns and
// potentially zero-width repetition bodies throw LRDiagBytePEGError.
class LRDiagBytePEG {
public:
    explicit LRDiagBytePEG(std::string_view pattern)
        : peg_(std::make_shared<const PEG>(Compiler(pattern).compile())) {}

    // Returns the absolute exclusive end, including begin for a successful
    // zero-width match. Failure or an anchor past EOF returns nullopt.
    std::optional<std::size_t> matchPrefixAt(std::string_view source, std::size_t begin = 0) const {
        if (begin > source.size()) return std::nullopt;
        return peg_->matchPrefixUnchecked(Input{source}, begin);
    }

    // The caller may share this budget across rules and retries. Exhaustion
    // is distinct from a pattern mismatch.
    std::optional<std::size_t> matchPrefixAt(std::string_view source, std::size_t begin,
                                            std::size_t& remaining) const {
        if (begin > source.size()) return std::nullopt;
        return peg_->matchPrefixUnchecked(Input{source}, begin, &remaining);
    }

private:
    static constexpr std::size_t byteCount = 256;
    static constexpr unsigned maxNesting = 128;
    using Expr = PEGExpression;

    struct Alphabet {
        Grammar grammar;
        std::array<Symbol, byteCount> bytes{};
        Symbol start;

        Alphabet() {
            for (std::size_t byte = 0; byte < bytes.size(); ++byte)
                bytes[byte] = grammar.terminal(std::to_string(byte));
            start = grammar.nonterminal("byte-pattern");
            grammar.setStart(start);
        }
    };

    static const Alphabet& alphabet() {
        static const Alphabet value;
        return value;
    }

    struct Input {
        std::string_view bytes;

        std::size_t size() const noexcept { return bytes.size(); }
        Symbol operator[](std::size_t pos) const {
            return alphabet().bytes[static_cast<unsigned char>(bytes[pos])];
        }
    };

    struct Part {
        Expr expression;
        bool mayMatchEmpty;
    };

    class Compiler {
    public:
        explicit Compiler(std::string_view pattern) : pattern_(pattern) {}

        PEG compile() {
            auto root = choice(0);
            if (pos_ != pattern_.size()) fail("unexpected character");
            const auto& symbols = alphabet();
            return PEG(symbols.grammar, {{symbols.start, std::move(root.expression)}});
        }

    private:
        static bool whitespace(int ch) {
            return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
        }

        int peek(std::size_t ahead = 0) const {
            return ahead < pattern_.size() - pos_ ? static_cast<unsigned char>(pattern_[pos_ + ahead]) : -1;
        }

        void skipSpace() {
            while (whitespace(peek())) ++pos_;
        }

        [[noreturn]] void fail(const char* message) const { throw LRDiagBytePEGError(pos_, message); }

        Part choice(unsigned depth) {
            auto first = sequence(depth);
            if (peek() != '/') return first;
            bool mayMatchEmpty = first.mayMatchEmpty;
            std::vector<Expr> alternatives;
            alternatives.push_back(std::move(first.expression));
            while (peek() == '/') {
                ++pos_;
                auto part = sequence(depth);
                mayMatchEmpty = mayMatchEmpty || part.mayMatchEmpty;
                alternatives.push_back(std::move(part.expression));
            }
            return {Expr::choice(std::move(alternatives)), mayMatchEmpty};
        }

        Part sequence(unsigned depth) {
            skipSpace();
            std::vector<Expr> terms;
            bool mayMatchEmpty = true;
            while (peek() != -1 && peek() != '/' && peek() != ')') {
                auto part = prefix(depth);
                mayMatchEmpty = mayMatchEmpty && part.mayMatchEmpty;
                terms.push_back(std::move(part.expression));
                skipSpace();
            }
            if (terms.empty()) fail("expected an expression (use '' for an empty match)");
            if (terms.size() == 1) return {std::move(terms.front()), mayMatchEmpty};
            return {Expr::sequence(std::move(terms)), mayMatchEmpty};
        }

        Part prefix(unsigned depth) {
            if (depth >= maxNesting) fail("expression nesting limit exceeded");
            const auto op = peek();
            if (op == '&' || op == '!') {
                ++pos_;
                skipSpace();
                auto part = prefix(depth + 1);
                return {op == '&' ? Expr::andPredicate(std::move(part.expression))
                                 : Expr::notPredicate(std::move(part.expression)), true};
            }
            auto part = atom(depth);
            skipSpace();
            switch (peek()) {
            case '?':
                ++pos_;
                return {Expr::optional(std::move(part.expression)), true};
            case '*':
            case '+': {
                if (part.mayMatchEmpty) fail("repetition body may match without consuming input");
                const auto op = peek();
                ++pos_;
                return {op == '*' ? Expr::repeat(std::move(part.expression))
                                  : Expr::oneOrMore(std::move(part.expression)), op == '*'};
            }
            default:
                return part;
            }
        }

        Part atom(unsigned depth) {
            switch (peek()) {
            case '(': {
                ++pos_;
                auto part = choice(depth + 1);
                if (peek() != ')') fail("expected ')'");
                ++pos_;
                return part;
            }
            case '\'':
            case '"':
                return literal();
            case '[':
                return byteClass();
            case '.': {
                ++pos_;
                const auto& bytes = alphabet().bytes;
                static const auto any = Expr::tokenSet({bytes.begin(), bytes.end()});
                return {any, false};
            }
            default:
                fail("expected a literal, byte class, '.', or group");
            }
        }

        Part literal() {
            const auto quote = peek();
            ++pos_;
            std::vector<Expr> terms;
            while (peek() != quote) {
                if (peek() == -1) fail("unterminated literal");
                terms.push_back(Expr::token(alphabet().bytes[byte()]));
            }
            ++pos_;
            if (terms.size() == 1) return {std::move(terms.front()), false};
            const bool empty = terms.empty();
            return {Expr::sequence(std::move(terms)), empty};
        }

        Part byteClass() {
            ++pos_; // '['
            const bool inverted = peek() == '^';
            if (inverted) ++pos_;
            if (peek() == ']') fail("empty byte class");
            std::array<bool, byteCount> included{};
            while (peek() != ']') {
                if (peek() == -1) fail("unterminated byte class");
                const auto first = byte();
                auto last = first;
                if (peek() == '-' && peek(1) != ']' && peek(1) != -1) {
                    ++pos_;
                    const auto lastPos = pos_;
                    last = byte();
                    if (last < first) throw LRDiagBytePEGError(lastPos, "descending byte range");
                }
                for (auto b = first; b <= last; ++b) included[b] = true;
            }
            ++pos_;
            std::vector<Symbol> symbols;
            for (std::size_t b = 0; b < included.size(); ++b)
                if (included[b] != inverted) symbols.push_back(alphabet().bytes[b]);
            return {Expr::tokenSet(std::move(symbols)), false};
        }

        unsigned byte() {
            const auto ch = peek();
            ++pos_;
            if (ch != '\\') return ch;
            const auto escape = peek();
            switch (escape) {
            case '0': ++pos_; return '\0';
            case 'a': ++pos_; return '\a';
            case 'b': ++pos_; return '\b';
            case 'f': ++pos_; return '\f';
            case 'n': ++pos_; return '\n';
            case 'r': ++pos_; return '\r';
            case 't': ++pos_; return '\t';
            case 'v': ++pos_; return '\v';
            case 'x': {
                ++pos_;
                const auto high = hexDigit();
                return high * 16 + hexDigit();
            }
            default:
                if ((escape >= '!' && escape <= '/') || (escape >= ':' && escape <= '@') ||
                    (escape >= '[' && escape <= '`') || (escape >= '{' && escape <= '~')) {
                    ++pos_;
                    return escape;
                }
                fail("invalid byte escape");
            }
        }

        unsigned hexDigit() {
            const auto ch = peek();
            unsigned value;
            if (ch >= '0' && ch <= '9') value = ch - '0';
            else if (ch >= 'a' && ch <= 'f') value = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') value = ch - 'A' + 10;
            else fail("expected two hex digits after '\\x'");
            ++pos_;
            return value;
        }

        std::string_view pattern_;
        std::size_t pos_ = 0;
    };

    std::shared_ptr<const PEG> peg_;
};

} // namespace firestone::lr
