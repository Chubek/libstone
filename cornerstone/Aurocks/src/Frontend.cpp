#include "aurocks/Compiler.hpp"

#include <unordered_set>
#include <utility>

namespace aurocks::detail {
namespace {

bool letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool digit(char c) { return c >= '0' && c <= '9'; }
bool nameChar(char c) { return letter(c) || digit(c); }
bool newline(char c) { return c == '\n' || c == '\r'; }
bool horizontal(char c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v'; }
bool space(char c) { return horizontal(c) || newline(c); }

std::string_view trim(std::string_view text) {
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}

struct ParseFailure {};

class Reader {
public:
    Reader(std::string_view source, std::vector<Diagnostic>& diagnostics)
        : source_(source), diagnostics_(diagnostics) {}

    Grammar read() {
        Grammar grammar;
        Grammar fallback;
        bool productionSection = false;
        bool hasPrologue = false;
        while (true) {
            skipTrivia();
            if (atEnd()) break;
            if (starts("%%")) {
                advance(2);
                if (productionSection) {
                    grammar.epilogue = source_.substr(cursor_.offset);
                    break;
                }
                productionSection = true;
            } else if (starts("%{")) {
                if (hasPrologue || productionSection)
                    fail(cursor_, "syntax.section", "misplaced C prologue");
                grammar.prologue = readCode(&fallback);
                hasPrologue = true;
            } else if (peek() == '%') {
                readGlobal(grammar);
            } else if (letter(peek())) {
                productionSection = true;
                readProduction(grammar);
            } else {
                fail(cursor_, "syntax.production", "expected a directive or production name");
            }
        }
        if (!fallback.directives.empty()) {
            std::unordered_set<std::string> defined;
            for (const auto& directive : grammar.directives) defined.insert(directive.name);
            for (auto& directive : fallback.directives) {
                if (!defined.insert(directive.name).second) continue;
                if (directive.name == "start") grammar.start = std::move(fallback.start);
                if (directive.name == "skip") grammar.skip = std::move(fallback.skip);
                grammar.directives.push_back(std::move(directive));
            }
        }
        if (grammar.start.empty())
            fail({}, "syntax.start", "missing %start directive");
        if (grammar.rules.empty())
            fail(cursor_, "syntax.production", "no productions found");
        return grammar;
    }

private:
    enum class DirectiveScope { global, header, alternative };

    std::string_view source_;
    std::vector<Diagnostic>& diagnostics_;
    Location cursor_;

    bool atEnd() const { return cursor_.offset == source_.size(); }

    char peek(std::size_t distance = 0) const {
        return distance < source_.size() - cursor_.offset ? source_[cursor_.offset + distance] : '\0';
    }

    bool starts(std::string_view text) const {
        return source_.substr(cursor_.offset).starts_with(text);
    }

    char take() {
        const auto offset = cursor_.offset++;
        const char c = source_[offset];
        if (c == '\r' || (c == '\n' && (offset == 0 || source_[offset - 1] != '\r'))) {
            ++cursor_.line;
            cursor_.column = 1;
        } else if (c != '\n') {
            ++cursor_.column;
        }
        return c;
    }

    void advance(std::size_t count) {
        while (count-- != 0) take();
    }

    std::string_view from(std::size_t begin) const {
        return source_.substr(begin, cursor_.offset - begin);
    }

    bool atLineStart() const {
        auto i = cursor_.offset;
        while (i != 0 && horizontal(source_[i - 1])) --i;
        return i == 0 || newline(source_[i - 1]);
    }

    [[noreturn]] void fail(Location at, std::string code, std::string message) const {
        diagnostics_.push_back({Severity::error, at, std::move(code), std::move(message)});
        throw ParseFailure{};
    }

    bool lineSplice() {
        if (peek() != '\\' || !newline(peek(1))) return false;
        const bool crlf = peek(1) == '\r' && peek(2) == '\n';
        advance(crlf ? 3 : 2);
        return true;
    }

    bool skipComment(bool cCode = false) {
        if (starts("//")) {
            advance(2);
            while (!atEnd() && !newline(peek())) {
                if (!cCode || !lineSplice()) take();
            }
            return true;
        }
        if (!starts("/*")) return false;
        const Location begin = cursor_;
        advance(2);
        while (!atEnd() && !starts("*/")) take();
        if (atEnd()) fail(begin, "syntax.comment", "unterminated block comment");
        advance(2);
        return true;
    }

    void skipTrivia(bool sameLine = false) {
        const auto line = cursor_.line;
        while (!atEnd() && (!sameLine || cursor_.line == line)) {
            if (horizontal(peek()) || (!sameLine && newline(peek()))) take();
            else if (!skipComment()) break;
        }
    }

    std::string readName() {
        const auto begin = cursor_.offset;
        while (nameChar(peek())) take();
        return std::string(from(begin));
    }

    std::string readQuoted(bool cCode = false) {
        const Location begin = cursor_;
        const char quote = take();
        std::string decoded;
        while (!atEnd()) {
            if (cCode && lineSplice()) continue;
            const char c = take();
            if (c == quote) return decoded;
            if (newline(c)) break;
            if (c != '\\') {
                if (!cCode) decoded += c;
                continue;
            }
            if (atEnd() || newline(peek())) break;
            const char escaped = take();
            if (cCode) continue;
            // The legacy literal language unescapes only quotes and backslashes.
            if (escaped != '\\' && escaped != '\'' && escaped != '"') decoded += '\\';
            decoded += escaped;
        }
        fail(begin, "syntax.quote", cCode ? "unterminated C string or character literal"
                                         : "unterminated quoted terminal");
    }

    std::string readCode(Grammar* fallback = nullptr) {
        const bool prologue = fallback != nullptr;
        const Location opening = cursor_;
        advance(prologue ? 2 : 1);
        const auto begin = cursor_.offset;
        std::size_t depth = 0;
        std::vector<std::pair<std::size_t, std::size_t>> declarations;
        while (!atEnd()) {
            if (skipComment(true) || lineSplice()) continue;
            if (peek() == '"' || peek() == '\'') {
                readQuoted(true);
                continue;
            }
            if (prologue ? starts("%}") : (peek() == '}' && depth == 0)) {
                std::string text(from(begin));
                // Fallback declarations are grammar metadata, not C. Blank
                // their text without changing the prologue's line layout.
                for (const auto& [first, last] : declarations) {
                    for (auto i = first - begin; i < last - begin; ++i) {
                        if (!newline(text[i])) text[i] = ' ';
                    }
                }
                advance(prologue ? 2 : 1);
                return text;
            }
            if (prologue && depth == 0 && peek() == '%' && letter(peek(1)) &&
                (cursor_.offset == begin || atLineStart())) {
                std::size_t end = 2;
                while (nameChar(peek(end))) ++end;
                if (space(peek(end)) || peek(end) == '\0') {
                    const auto first = cursor_.offset;
                    readGlobal(*fallback);
                    declarations.emplace_back(first, cursor_.offset);
                    continue;
                }
            }
            if (peek() == '{') ++depth;
            if (peek() == '}' && depth != 0) --depth;
            take();
        }
        fail(opening, prologue ? "syntax.prologue" : "syntax.action",
             prologue ? "unterminated C prologue; expected '%}'" : "unterminated action; expected '}'");
    }

    void readRegexEscape(Location opening) {
        take(); // '\'
        if (atEnd() || newline(peek()))
            fail(opening, "syntax.regex", "incomplete regular-expression escape");
        const char escape = take();
        if (escape != 'x' && escape != 'X') return;
        const auto hexDigit = [](char c) {
            return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        };
        if (!hexDigit(peek()))
            fail(opening, "syntax.regex", "hexadecimal regular-expression escape needs a digit");
        take();
        if (hexDigit(peek())) take();
    }

    void readBracket(Location opening) {
        take(); // '['
        if (peek() == '^') take();
        if (peek() == ']') take(); // ']' is a member in the first position.
        while (!atEnd() && !newline(peek())) {
            if (peek() == ']') {
                take();
                return;
            }
            if (peek() == '\\') {
                readRegexEscape(cursor_);
            } else if (peek() == '[' && (peek(1) == ':' || peek(1) == '=' || peek(1) == '.')) {
                const Location at = cursor_;
                take();
                const char marker = take();
                const auto begin = cursor_.offset;
                while (!atEnd() && !newline(peek()) && !(peek() == marker && peek(1) == ']')) take();
                const auto name = from(begin);
                if (atEnd() || newline(peek()) || name.empty())
                    fail(at, "syntax.regex", "malformed POSIX bracket element");
                if (marker == ':') {
                    constexpr std::string_view classes[] = {
                        "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower",
                        "print", "punct", "space", "upper", "xdigit", "word"
                    };
                    std::string lower(name);
                    for (char& c : lower) {
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    }
                    bool known = false;
                    for (const auto candidate : classes) known |= candidate == lower;
                    if (!known)
                        fail(at, "syntax.regex", "unknown POSIX character class '" + std::string(name) + "'");
                }
                advance(2);
            } else {
                take();
            }
        }
        fail(opening, "syntax.regex", "unterminated regular-expression bracket");
    }

    bool startsInterval() const {
        std::size_t i = 1;
        if (!digit(peek(i))) return false;
        while (digit(peek(i))) ++i;
        if (peek(i) == ',') {
            ++i;
            while (digit(peek(i))) ++i;
        }
        return peek(i) == '}';
    }

    // Only use brace syntax as an interval when its complete shape matches.
    void readInterval(Location opening) {
        take(); // '{'
        const auto minStart = cursor_.offset;
        while (digit(peek())) take();
        auto min = from(minStart);
        if (peek() == ',') {
            take();
            const auto maxStart = cursor_.offset;
            while (digit(peek())) take();
            auto max = from(maxStart);
            if (!max.empty()) {
                while (!min.empty() && min.front() == '0') min.remove_prefix(1);
                while (!max.empty() && max.front() == '0') max.remove_prefix(1);
                if (min.size() > max.size() || (min.size() == max.size() && min > max))
                    fail(opening, "syntax.regex", "regular-expression interval has a reversed range");
            }
        }
        take(); // '}'
    }

    std::string readRegex() {
        const Location opening = cursor_;
        take(); // '/'
        const auto begin = cursor_.offset;
        std::vector<Location> groups;
        bool canRepeat = false;
        while (!atEnd() && !newline(peek())) {
            const Location at = cursor_;
            switch (peek()) {
            case '/': {
                if (!groups.empty())
                    fail(groups.back(), "syntax.regex", "unclosed regular-expression group");
                std::string text(from(begin));
                take();
                return text;
            }
            case '[':
                readBracket(at);
                canRepeat = true;
                break;
            case '\\':
                readRegexEscape(at);
                canRepeat = true;
                break;
            case '(':
                groups.push_back(at);
                take();
                canRepeat = false;
                break;
            case ')':
                if (groups.empty()) fail(at, "syntax.regex", "unmatched ')' in regular expression");
                groups.pop_back();
                take();
                canRepeat = true;
                break;
            case '|':
                take();
                canRepeat = false;
                break;
            case '*':
            case '+':
            case '?':
                if (!canRepeat) fail(at, "syntax.regex", "regular-expression repeat has no operand");
                take();
                break;
            case '{':
                if (canRepeat && startsInterval()) {
                    readInterval(at);
                    break;
                }
                [[fallthrough]];
            default:
                take();
                canRepeat = true;
                break;
            }
        }
        fail(opening, "syntax.regex", "unterminated regular expression");
    }

    Directive readDirectiveName() {
        const Location at = cursor_;
        take(); // '%'
        if (!letter(peek())) fail(at, "syntax.directive", "expected a directive name after '%'");
        auto name = readName();
        for (char& c : name) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return {std::move(name), {}, at};
    }

    // Metadata may contain slash-delimited arguments as well as ordinary paths.
    // Only take the slash-delimited form when a matching delimiter is visible.
    std::size_t directiveRegexLength() const {
        std::size_t i = 1;
        while (peek(i) != '\0' && !newline(peek(i))) {
            if (peek(i) == '/') {
                const char next = peek(i + 1);
                return next == '\0' || space(next) || next == ',' || next == ')' ||
                       next == ':' || next == ';' || next == '%' || next == '|' || next == '{'
                           ? i + 1 : 0;
            }
            if (peek(i) == '\\') {
                if (peek(i + 1) == '\0' || newline(peek(i + 1))) return 0;
                i += 2;
            } else if (peek(i) == '[') {
                ++i;
                if (peek(i) == '^') ++i;
                if (peek(i) == ']') ++i;
                while (peek(i) != ']') {
                    if (peek(i) == '\0' || newline(peek(i))) return 0;
                    if (peek(i) == '\\') {
                        if (peek(i + 1) == '\0' || newline(peek(i + 1))) return 0;
                        i += 2;
                    } else if (peek(i) == '[' && (peek(i + 1) == ':' || peek(i + 1) == '=' || peek(i + 1) == '.')) {
                        const char marker = peek(i + 1);
                        i += 2;
                        while (!(peek(i) == marker && peek(i + 1) == ']')) {
                            if (peek(i) == '\0' || newline(peek(i))) return 0;
                            ++i;
                        }
                        i += 2;
                    } else {
                        ++i;
                    }
                }
                ++i;
            } else {
                ++i;
            }
        }
        return 0;
    }

    // Metadata is opaque. Quotes and grouped arguments can contain delimiters;
    // those delimiters are only structural at the outermost level.
    std::string readDirectiveValue(DirectiveScope scope) {
        const auto begin = cursor_.offset;
        auto end = begin;
        std::vector<char> groups;
        const auto line = cursor_.line;
        while (!atEnd() && cursor_.line == line && !newline(peek())) {
            if (horizontal(peek())) {
                take();
            } else if (skipComment()) {
                continue;
            } else if (peek() == '"' || peek() == '\'') {
                readQuoted();
                end = cursor_.offset;
            } else {
                const char c = peek();
                if (c == '/') {
                    const auto length = directiveRegexLength();
                    if (length != 0) {
                        advance(length);
                        end = cursor_.offset;
                        continue;
                    }
                }
                if (groups.empty()) {
                    if (c == ';' || starts("%%") || starts("%{") || starts("%}")) break;
                    if (scope == DirectiveScope::header && c == ':') break;
                    if (scope != DirectiveScope::global && c == '%' && letter(peek(1))) break;
                    if (scope == DirectiveScope::alternative && (c == '|' || c == '{')) break;
                }
                if (c == '(' || c == '[') groups.push_back(c == '(' ? ')' : ']');
                else if (!groups.empty() && c == groups.back()) groups.pop_back();
                take();
                end = cursor_.offset;
            }
        }
        return std::string(trim(source_.substr(begin, end - begin)));
    }

    void readGlobal(Grammar& grammar) {
        auto directive = readDirectiveName();
        skipTrivia(true);
        const auto arg = cursor_;
        if (directive.name == "start" || directive.name == "skip") {
            if (directive.name == "start") {
                if (arg.line != directive.location.line || !letter(peek()))
                    fail(arg, "syntax.start", "expected a production name after %start");
                directive.value = readName();
                if (grammar.start.empty()) grammar.start = directive.value;
            } else {
                if (arg.line != directive.location.line || peek() != '/')
                    fail(arg, "syntax.skip", "expected a slash-delimited regular expression after %skip");
                auto pattern = readRegex();
                if (!grammar.skip) grammar.skip = std::move(pattern);
                directive.value = from(arg.offset);
            }
            skipTrivia(true);
            if (cursor_.line == arg.line && !atEnd() && !newline(peek()) && peek() != ';' &&
                peek() != '%')
                fail(cursor_, "syntax.directive", "unexpected text after %" + directive.name + " argument");
        } else {
            if (directive.name == "empty")
                fail(directive.location, "syntax.empty", "%empty is only valid in a production alternative");
            if (arg.line == directive.location.line)
                directive.value = readDirectiveValue(DirectiveScope::global);
        }
        if (peek() == ';') take();
        grammar.directives.push_back(std::move(directive));
    }

    void readProduction(Grammar& grammar) {
        const Location production = cursor_;
        const auto lhs = readName();
        skipTrivia();
        std::vector<Directive> header;
        while (peek() == '%' && letter(peek(1))) {
            auto directive = readDirectiveName();
            directive.value = readDirectiveValue(DirectiveScope::header);
            header.push_back(std::move(directive));
            skipTrivia();
        }
        if (peek() != ':')
            fail(cursor_, "syntax.production", "expected ':' after production '" + lhs + "'");
        take();
        auto location = production;
        while (true) {
            Rule rule{lhs, {}, {}, header, location};
            const bool nextProduction = readAlternative(rule);
            grammar.rules.push_back(std::move(rule));
            // V2 also finalized a production at the next line-leading rule or
            // at the end of the section, even if the semicolon was omitted.
            if (nextProduction || atEnd() || starts("%%")) return;
            if (peek() != '|') break;
            take();
            skipTrivia();
            location = cursor_;
        }
        if (peek() != ';')
            fail(cursor_, "syntax.production", "expected ';' after production '" + lhs + "'");
        take();
    }

    bool beginsLegacyProduction() const {
        if (!letter(peek()) || !atLineStart()) return false;
        std::size_t i = 1;
        while (nameChar(peek(i))) ++i;
        while (horizontal(peek(i))) ++i;
        while (peek(i) == '%' && letter(peek(i + 1))) {
            i += 2;
            while (nameChar(peek(i))) ++i;
            while (horizontal(peek(i))) ++i;
        }
        return peek(i) == ':';
    }

    bool readAlternative(Rule& rule) {
        bool hasAction = false;
        while (true) {
            skipTrivia();
            const Location at = cursor_;
            const bool nextProduction = beginsLegacyProduction();
            if (nextProduction || atEnd() || starts("%%") || peek() == '|' || peek() == ';') {
                if (!hasAction && rule.rhs.empty())
                    fail(at, "syntax.empty", "expected a grammar symbol or %empty");
                return nextProduction;
            }
            if (peek() == '{') {
                if (hasAction) fail(at, "syntax.action", "only one action is allowed per alternative");
                rule.action = readCode();
                hasAction = true;
                continue;
            }
            if (peek() == '%') {
                auto directive = readDirectiveName();
                if (directive.name != "empty") {
                    if (directive.name == "dprec") {
                        skipTrivia();
                        const auto begin = cursor_.offset;
                        if (peek() == '-') take();
                        if (!digit(peek()))
                            fail(cursor_, "syntax.dprec", "%dprec requires a signed integer");
                        while (digit(peek())) take();
                        if (letter(peek()))
                            fail(cursor_, "syntax.dprec", "unexpected text after %dprec integer");
                        directive.value = from(begin);
                    } else {
                        directive.value = readDirectiveValue(DirectiveScope::alternative);
                    }
                    rule.directives.push_back(std::move(directive));
                    continue;
                }
                if (hasAction || !rule.rhs.empty())
                    fail(at, "syntax.empty", "%empty must be the only symbol of an alternative");
                rule.rhs.push_back({SymbolKind::empty, {}, at});
                continue;
            }
            if (hasAction) fail(at, "syntax.action", "expected '|' or ';' after action");
            if (!rule.rhs.empty() && rule.rhs.front().kind == SymbolKind::empty)
                fail(at, "syntax.empty", "%empty must be the only symbol of an alternative");
            if (peek() == '\'' || peek() == '"') {
                rule.rhs.push_back({SymbolKind::literal, readQuoted(), at});
            } else if (peek() == '/') {
                rule.rhs.push_back({SymbolKind::regex, readRegex(), at});
            } else if (letter(peek())) {
                auto name = readName();
                skipTrivia();
                if (peek() == ':')
                    fail(at, "syntax.production", "missing ';' before production '" + name + "'");
                rule.rhs.push_back({SymbolKind::nonterminal, std::move(name), at});
            } else {
                fail(at, "syntax.symbol", "expected a grammar symbol, directive, action, '|' or ';'");
            }
        }
    }
};

} // namespace

std::optional<Grammar> parse(std::string_view source, std::vector<Diagnostic>& diagnostics) {
    try {
        return Reader(source, diagnostics).read();
    } catch (const ParseFailure&) {
        return std::nullopt;
    }
}

} // namespace aurocks::detail
