#pragma once
#include "DFA.hpp"
#include "DFAMinimization.hpp"
#include "ThompsonConstruction.hpp"
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace firestone::lr {

struct LexerRule {
    std::string pattern;
    std::size_t token;
    bool skip;

    LexerRule(std::string_view pattern, std::size_t token, bool skip = false)
        : pattern(pattern), token(token), skip(skip) {}
};

enum class LexStatus { Error = -1, End = 0, Token = 1 };

struct LexToken {
    LexStatus status;
    std::size_t token;  // Zero for Error and End; zero is also a valid token ID.
    std::size_t offset; // Byte position in the original input.
    std::size_t length;

    friend bool operator==(const LexToken& a, const LexToken& b) {
        return a.status == b.status && a.token == b.token && a.offset == b.offset && a.length == b.length;
    }
};

namespace detail {

inline void require_nonnullable(const DFA& dfa) {
    const auto& acceptance = dfa.states().at(dfa.start()).acceptance;
    if (acceptance)
        throw std::invalid_argument("lexer: empty match at rule priority " + std::to_string(acceptance->priority));
}

// Shared source-generation policy: portable external C identifiers (31 significant
// characters), no reserved leading underscore or user-supplied source fragments.
inline void require_scanner_name(std::string_view name) {
    auto letter = [](char c) {
        return std::string_view("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ").find(c) != std::string_view::npos;
    };
    if (name.empty() || name.size() > 31 || !letter(name.front()))
        throw std::invalid_argument("lexer codegen: invalid scanner name");
    for (char c : name)
        if (!letter(c) && c != '_' && !(c >= '0' && c <= '9'))
            throw std::invalid_argument("lexer codegen: invalid scanner name");
    constexpr std::string_view reserved[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
        "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register",
        "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef",
        "union", "unsigned", "void", "volatile", "while", "main", "size_t", "ptrdiff_t", "wchar_t",
        "NULL", "offsetof", "CHAR_BIT", "CHAR_MAX", "CHAR_MIN", "SCHAR_MAX", "SCHAR_MIN", "UCHAR_MAX",
        "SHRT_MIN", "SHRT_MAX", "USHRT_MAX", "INT_MIN", "INT_MAX", "UINT_MAX", "LONG_MIN", "LONG_MAX",
        "ULONG_MAX", "LLONG_MIN", "LLONG_MAX", "ULLONG_MAX", "MB_LEN_MAX"
    };
    for (auto word : reserved)
        if (name == word) throw std::invalid_argument("lexer codegen: reserved scanner name");
}

template<class UInt>
void require_codegen_id(UInt id) {
    if constexpr (std::numeric_limits<UInt>::digits > 64) {
        if (id > 18446744073709551615ULL)
            throw std::length_error("lexer codegen: ID wider than 64 bits");
    }
}

template<class MatchPrefix>
LexToken lexer_next(std::string_view input, std::size_t& offset, MatchPrefix match_prefix) {
    if (offset > input.size()) throw std::out_of_range("lexer: cursor past input");
    while (offset < input.size()) {
        auto begin = offset;
        auto match = match_prefix(input.substr(begin));
        if (!match) {
            ++offset;
            return {LexStatus::Error, 0, begin, 1};
        }
        offset += match->length;
        if (!match->acceptance.skip)
            return {LexStatus::Token, match->acceptance.token, begin, match->length};
    }
    return {LexStatus::End, 0, offset, 0};
}

} // namespace detail

class Lexer {
    DFA dfa_;

public:
    Lexer() = default;
    explicit Lexer(DFA dfa) : dfa_(std::move(dfa)) { detail::require_nonnullable(dfa_); }

    const DFA& dfa() const { return dfa_; }

    // Scan from offset, discarding skip rules. Longest match wins; equal lengths
    // use rule priority. Unknown input consumes one byte; EOF leaves offset at size.
    LexToken next(std::string_view input, std::size_t& offset) const {
        return detail::lexer_next(input, offset, [&](std::string_view tail) { return dfa_.match_prefix(tail); });
    }
};

inline Lexer make_lexer(const std::vector<LexerRule>& rules, bool minimize_dfa = true) {
    NFA nfa;
    for (std::size_t priority = 0; priority < rules.size(); ++priority) {
        const auto& rule = rules[priority];
        auto part = detail::ThompsonParser(nfa, rule.pattern).parse();
        nfa.add_epsilon(nfa.start(), part.entry);
        nfa.set_acceptance(part.exit, Acceptance{rule.token, priority, rule.skip});
    }
    auto dfa = determinize(nfa);
    detail::require_nonnullable(dfa);
    return Lexer(minimize_dfa ? minimize(dfa) : std::move(dfa));
}

class LexerGenerator {
    std::vector<LexerRule> rules_;

public:
    LexerGenerator() = default;
    explicit LexerGenerator(std::vector<LexerRule> rules) : rules_(std::move(rules)) {}

    LexerGenerator& add_rule(LexerRule rule) {
        rules_.push_back(std::move(rule));
        return *this;
    }

    Lexer build(bool minimize_dfa = true) const { return make_lexer(rules_, minimize_dfa); }

    static Lexer build(const std::vector<LexerRule>& rules, bool minimize_dfa = true) {
        return make_lexer(rules, minimize_dfa);
    }

    static Lexer build(std::initializer_list<LexerRule> rules, bool minimize_dfa = true) {
        return make_lexer(rules, minimize_dfa);
    }
};

} // namespace firestone::lr
