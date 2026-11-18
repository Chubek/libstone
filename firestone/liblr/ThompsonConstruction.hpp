#pragma once
#include "NFA.hpp"
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace firestone::lr {

class RegexError : public std::invalid_argument {
    std::size_t offset_;

public:
    RegexError(std::size_t offset, const char* message)
        : std::invalid_argument("regex byte " + std::to_string(offset) + ": " + message), offset_(offset) {}

    std::size_t offset() const { return offset_; }
};

namespace detail {

struct ThompsonFragment {
    StateId entry;
    StateId exit;
};

class ThompsonParser {
    NFA& nfa_;
    std::string_view pattern_;
    std::size_t pos_ = 0;

    bool at(char c) const { return pos_ < pattern_.size() && pattern_[pos_] == c; }

    bool take(char c) {
        if (!at(c)) return false;
        ++pos_;
        return true;
    }

    [[noreturn]] void fail(const char* why) const { throw RegexError(pos_, why); }

    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        constexpr std::string_view lower = "abcdef", upper = "ABCDEF";
        auto value = lower.find(c);
        if (value == std::string_view::npos) value = upper.find(c);
        return value == std::string_view::npos ? -1 : static_cast<int>(value) + 10;
    }

    unsigned char read_byte() {
        if (pos_ == pattern_.size()) fail("expected a byte");
        const auto* bytes = reinterpret_cast<const unsigned char*>(pattern_.data());
        if (!take('\\')) return bytes[pos_++];
        if (pos_ == pattern_.size()) fail("unfinished escape");
        switch (pattern_[pos_]) {
            case 'n': ++pos_; return 10;
            case 'r': ++pos_; return 13;
            case 't': ++pos_; return 9;
            case '0': ++pos_; return 0;
            case 'x': {
                ++pos_;
                unsigned char result = 0;
                for (int i = 0; i < 2; ++i) {
                    if (pos_ == pattern_.size() || hex(pattern_[pos_]) < 0) fail("expected two hex digits");
                    result = static_cast<unsigned char>(16 * result + hex(pattern_[pos_++]));
                }
                return result;
            }
            default:
                if (std::string_view(R"(\.^$|()[]*+?{}-)").find(pattern_[pos_]) != std::string_view::npos)
                    return bytes[pos_++];
                fail("unknown escape");
        }
    }

    ByteSet character_class() {
        const bool negated = take('^');
        if (at(']')) fail("empty character class");
        ByteSet bytes;
        while (!take(']')) {
            auto first = read_byte();
            auto last = first;
            if (at('-') && pos_ + 1 < pattern_.size() && pattern_[pos_ + 1] != ']') {
                ++pos_;
                last = read_byte();
                if (last < first) fail("reversed character range");
            }
            for (unsigned int byte = first; byte <= last; ++byte) bytes.set(byte);
        }
        return negated ? ~bytes : bytes;
    }

    ThompsonFragment atom(unsigned int depth) {
        if (take('(')) {
            if (depth == 256) fail("group nesting limit exceeded");
            auto part = alternate(depth + 1);
            if (!take(')')) fail("expected ')'");
            return part;
        }
        ByteSet bytes;
        if (take('[')) bytes = character_class();
        else if (take('.')) bytes.set();
        else {
            if (at('*') || at('+') || at('?')) fail("quantifier has no operand");
            if (at(']') || at('^') || at('$') || at('{') || at('}')) fail("unsupported or unescaped metacharacter");
            bytes.set(read_byte());
        }
        ThompsonFragment part{nfa_.add_state(), nfa_.add_state()};
        nfa_.add_transition(part.entry, bytes, part.exit);
        return part;
    }

    ThompsonFragment repeat(unsigned int depth) {
        auto part = atom(depth);
        if (!at('*') && !at('+') && !at('?')) return part;
        auto quantifier = pattern_[pos_++];
        ThompsonFragment result{nfa_.add_state(), nfa_.add_state()};
        nfa_.add_epsilon(result.entry, part.entry);
        nfa_.add_epsilon(part.exit, result.exit);
        if (quantifier != '+') nfa_.add_epsilon(result.entry, result.exit);
        if (quantifier != '?') nfa_.add_epsilon(part.exit, part.entry);
        if (at('*') || at('+') || at('?')) fail("stacked quantifiers");
        return result;
    }

    ThompsonFragment concatenate(unsigned int depth) {
        if (pos_ == pattern_.size() || at(')') || at('|')) {
            auto epsilon = nfa_.add_state();
            return {epsilon, epsilon};
        }
        auto part = repeat(depth);
        while (pos_ < pattern_.size() && !at(')') && !at('|')) {
            auto next = repeat(depth);
            nfa_.add_epsilon(part.exit, next.entry);
            part.exit = next.exit;
        }
        return part;
    }

    ThompsonFragment alternate(unsigned int depth) {
        auto part = concatenate(depth);
        if (!take('|')) return part;
        ThompsonFragment result{nfa_.add_state(), nfa_.add_state()};
        do {
            nfa_.add_epsilon(result.entry, part.entry);
            nfa_.add_epsilon(part.exit, result.exit);
            part = concatenate(depth);
        } while (take('|'));
        nfa_.add_epsilon(result.entry, part.entry);
        nfa_.add_epsilon(part.exit, result.exit);
        return result;
    }

public:
    ThompsonParser(NFA& nfa, std::string_view pattern) : nfa_(nfa), pattern_(pattern) {}

    ThompsonFragment parse() {
        auto part = alternate(0);
        if (pos_ != pattern_.size()) fail("unexpected ')'");
        return part;
    }
};

} // namespace detail

// Byte regex, in increasing precedence: alternation |, concatenation, postfix * + ?.
// Supports () groups (max depth 256), . (any byte), [abc], [a-z], [^a-z].
// Class ranges are inclusive; ] is escaped to include it. '-' is a range separator
// when it follows a class item and precedes another byte, and literal otherwise.
// Escapes: \n, \r, \t, \0, \xHH (two hex digits), and \ before regex punctuation.
// Empty expressions/branches are epsilon. No anchors, counted repeats, or Unicode mode.
inline NFA compile_regex(std::string_view pattern, Acceptance acceptance = {}) {
    NFA nfa;
    auto part = detail::ThompsonParser(nfa, pattern).parse();
    nfa.add_epsilon(nfa.start(), part.entry);
    nfa.set_acceptance(part.exit, acceptance);
    return nfa;
}

} // namespace firestone::lr
