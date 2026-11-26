#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asdl::grammar {

enum class TokenKind { end, identifier, string, number, punct };

struct Token {
    TokenKind kind = TokenKind::end;
    std::string text;
    std::size_t at = 0;
};

constexpr bool lower(char c) { return c >= 'a' && c <= 'z'; }
constexpr bool upper(char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool alpha(char c) { return lower(c) || upper(c); }
constexpr bool word(char c) { return alpha(c) || digit(c) || c == '_'; }
constexpr bool horizontal(char c) { return c == ' ' || c == '\t'; }
constexpr bool newline(char c) { return c == '\r' || c == '\n'; }
constexpr bool space(char c) {
    return horizontal(c) || newline(c) || c == '\f' || c == '\v';
}

struct Span {
    std::size_t at;
    std::string text;
};
struct ScanError {
    std::size_t at;
    std::string message;
};

class Lexer {
public:
    explicit Lexer(std::string_view text) : input(text) {
        for (std::size_t i = 0; i < input.size(); ++i) {
            if (!newline(input[i])) continue;
            if (newline(ch(i + 1)) && ch(i + 1) != input[i]) ++i;
            lines.push_back(i + 1);
        }
    }

    std::size_t line(std::size_t at) const {
        return static_cast<std::size_t>(std::upper_bound(lines.begin(), lines.end(), at)
                                        - lines.begin());
    }
    std::size_t column(std::size_t at) const { return at - lines[line(at) - 1] + 1; }
    std::size_t line_start(std::size_t at) const { return lines[line(at) - 1]; }
    char ch(std::size_t at) const { return at < input.size() ? input[at] : '\0'; }
    bool starts(std::size_t at, std::string_view prefix) const {
        return input.substr(at, prefix.size()) == prefix;
    }
    std::size_t line_end(std::size_t at) const {
        while (at < input.size() && !newline(input[at])) ++at;
        return at;
    }
    std::size_t after_break(std::size_t at) const {
        if (!newline(ch(at))) return at;
        if (newline(ch(at + 1)) && ch(at + 1) != ch(at)) ++at;
        return at + 1;
    }
    [[noreturn]] static void fail(std::size_t at, std::string message) {
        throw ScanError{at, std::move(message)};
    }

    void skip() {
        while (pos < input.size()) {
            if (space(ch(pos))) ++pos;
            else if (starts(pos, "/*")) {
                auto end = input.find("*/", pos + 2);
                if (end == input.npos) fail(pos, "unterminated block comment");
                pos = end + 2;
            } else if (starts(pos, "--") || starts(pos, "//") ||
                       (ch(pos) == '#' && !boolean_enabled)) {
                pos = line_end(pos);
            } else {
                return;
            }
        }
    }

    Token next() {
        skip();
        auto at = pos;
        if (pos == input.size()) return {TokenKind::end, {}, at};
        auto c = input[pos++];
        if (alpha(c) || c == '_') {
            while (word(ch(pos))) ++pos;
            return token(TokenKind::identifier, at);
        }
        if (digit(c)) {
            while (digit(ch(pos))) ++pos;
            return token(TokenKind::number, at);
        }
        if (c == '"' || c == '\'') {
            std::string decoded;
            while (pos < input.size() && !newline(ch(pos))) {
                auto d = input[pos++];
                if (d == c) return {TokenKind::string, std::move(decoded), at};
                if (d == '\\') {
                    if (pos == input.size() || newline(ch(pos))) break;
                    d = input[pos++];
                    if (d == 'n') d = '\n';
                    else if (d == 'r') d = '\r';
                    else if (d == 't') d = '\t';
                }
                decoded.push_back(d);
            }
            fail(at, "unterminated string");
        }
        if (c == '#' && boolean_enabled) {
            while (word(ch(pos))) ++pos;
            return token(TokenKind::punct, at);
        }
        for (auto punct : {"::=", "%%", "%{", "->"}) {
            if (starts(at, punct)) {
                pos = at + std::string_view(punct).size();
                if (std::string_view(punct) == "%%" &&
                    (!blank(line_start(at), at) || !blank(pos, line_end(pos))))
                    fail(at, "%% must occupy a line by itself");
                return token(TokenKind::punct, at);
            }
        }
        if (std::string_view("{}()[]=|,+*?;.:<>!%").find(c) != input.npos)
            return token(TokenKind::punct, at);
        fail(at, "unexpected character '" + std::string(1, c) + "'");
    }

    bool blank(std::size_t start, std::size_t end) const {
        for (auto i = start; i < end; ++i) {
            if (!horizontal(input[i])) return false;
        }
        return true;
    }

    // Called directly after consuming %{, without a lookahead token.
    Span c_prologue(std::size_t opening) {
        auto start = pos;
        while (pos < input.size()) {
            if (starts(pos, "%}")) {
                auto end = pos;
                pos += 2;
                return span(start, end);
            }
            if (starts(pos, "/*")) {
                auto end = input.find("*/", pos + 2);
                if (end == input.npos) fail(pos, "unterminated C block comment");
                pos = end + 2;
            } else if (starts(pos, "//")) {
                std::size_t end;
                do {
                    end = line_end(pos);
                    pos = after_break(end);
                } while (pos < input.size() && end > 0 && ch(end - 1) == '\\');
            } else if (ch(pos) == '"' || ch(pos) == '\'') {
                quoted(false);
            } else {
                ++pos;
            }
        }
        fail(opening, "unterminated %{ prologue; expected %}");
    }

    // Lua's lexical state, not its syntax, determines where an .end is visible.
    // Resume ASDL tokenization at the terminating dot.
    Span lua_body(std::size_t opening) {
        auto start = pos;
        while (pos < input.size()) {
            if (starts(pos, ".end") && blank(line_start(pos), pos)) {
                auto tail = pos + 4;
                while (horizontal(ch(tail))) ++tail;
                if (ch(tail) == ';') {
                    ++tail;
                    while (horizontal(ch(tail))) ++tail;
                }
                if (tail == input.size() || newline(ch(tail)) || starts(tail, "--") ||
                    starts(tail, "//") || ch(tail) == '#')
                    return span(start, pos);
            }
            if (starts(pos, "--")) {
                pos += 2;
                auto level = long_open(pos);
                if (level == input.npos) pos = line_end(pos);
                else read_long(level);
            } else if (ch(pos) == '[' && long_open(pos) != input.npos) {
                read_long(long_open(pos));
            } else if (ch(pos) == '"' || ch(pos) == '\'') {
                quoted(true);
            } else {
                ++pos;
            }
        }
        fail(opening, "unterminated Lua logistics block; expected .end");
    }

    Span span(std::size_t start, std::size_t end) const {
        return {start, std::string(input.substr(start, end - start))};
    }

    std::string_view input;
    std::size_t pos = 0;
    bool boolean_enabled = false;

private:
    std::vector<std::size_t> lines{0};
    Token token(TokenKind kind, std::size_t at) const {
        return {kind, std::string(input.substr(at, pos - at)), at};
    }
    void quoted(bool lua) {
        auto at = pos;
        const char quote = input[pos++];
        while (pos < input.size()) {
            auto c = input[pos++];
            if (c == quote) return;
            if (newline(c)) break;
            if (c != '\\' || pos == input.size()) continue;
            if (newline(ch(pos))) pos = after_break(pos);
            else if (lua && ch(pos) == 'z') {
                ++pos;
                while (space(ch(pos))) ++pos;
            } else {
                ++pos;
            }
        }
        fail(at, lua ? "unterminated Lua short string" : "unterminated C literal in prologue");
    }
    std::size_t long_open(std::size_t at) const {
        if (ch(at) != '[') return input.npos;
        auto close = at + 1;
        while (ch(close) == '=') ++close;
        return ch(close) == '[' ? close - at - 1 : input.npos;
    }
    void read_long(std::size_t level) {
        std::string closing = "]" + std::string(level, '=') + "]";
        auto end = input.find(closing, pos + level + 2);
        if (end == input.npos) fail(pos, "unterminated Lua long bracket");
        pos = end + closing.size();
    }
};

} // namespace asdl::grammar
