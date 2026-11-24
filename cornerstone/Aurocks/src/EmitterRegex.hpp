#pragma once

#include <bitset>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aurocks::detail::portable {

using Bytes = std::bitset<256>;

struct Regex {
    enum Kind { bytes, begin, end, boundary, notBoundary, concat, alternate, repeat } kind;
    Bytes set;
    unsigned min = 0, max = 0;
    std::vector<std::unique_ptr<Regex>> children;

    explicit Regex(Kind k) : kind(k) {}
};

class RegexReader {
    using Node = std::unique_ptr<Regex>;
    static constexpr unsigned unlimited = 1025;
    std::string_view pattern;
    std::size_t pos = 0;

    int peek() const {
        return pos < pattern.size() ? static_cast<unsigned char>(pattern[pos]) : -1;
    }
    int take() {
        const auto c = peek();
        if (c < 0)
            throw std::runtime_error("unexpected end of expression");
        ++pos;
        return c;
    }
    bool accept(int c) {
        if (peek() != c)
            return false;
        ++pos;
        return true;
    }
    static bool digit(int c) { return c >= '0' && c <= '9'; }
    static Bytes characterClass(std::string_view written) {
        std::string name(written);
        for (char& c : name) {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        Bytes result;
        for (unsigned c = 0; c < 256; ++c) {
            const bool d = digit(c), l = c >= 'a' && c <= 'z', u = c >= 'A' && c <= 'Z';
            const bool graph = c >= 33 && c <= 126, blank = c == ' ' || c == '\t';
            if (name == "alnum" ? d || l || u :
                name == "alpha" ? l || u :
                name == "blank" ? blank :
                name == "cntrl" ? c < 32 || c == 127 :
                name == "digit" ? d :
                name == "graph" ? graph :
                name == "lower" ? l :
                name == "print" ? graph || c == ' ' :
                name == "punct" ? graph && !(d || l || u) :
                name == "space" ? blank || (c >= '\n' && c <= '\r') :
                name == "upper" ? u :
                name == "xdigit" ? d || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') :
                name == "word" ? d || l || u || c == '_' :
                throw std::runtime_error("unsupported character class"))
                result.set(c);
        }
        return result;
    }
    static int hex(int c) {
        if (digit(c)) return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    int escaped(int c) {
        if (c == 'x' || c == 'X') {
            const int first = hex(take());
            if (first < 0)
                throw std::runtime_error("hexadecimal escape requires a digit");
            const int second = hex(peek());
            return second < 0 ? first : (take(), 16 * first + second);
        }
        if (c >= '0' && c <= '7') {
            int value = c - '0';
            for (unsigned n = 1; n < 3 && peek() >= '0' && peek() <= '7'; ++n)
                value = 8 * value + (take() - '0');
            return value & 255;
        }
        switch (c) {
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case 'f': return '\f';
        case 'v': return '\v';
        case 'b': return '\b';
        case 'a': return '\a';
        case 'e': return 27;
        }
        return c;
    }
    static bool shorthand(int c, Bytes& set) {
        switch (c) {
        case 'd': case 'D': set = characterClass("digit"); break;
        case 's': case 'S': set = characterClass("space"); break;
        case 'w': case 'W': set = characterClass("word"); break;
        default: return false;
        }
        if (c == 'D' || c == 'S' || c == 'W')
            set.flip();
        return true;
    }
    int bracketAtom(Bytes& set) {
        if (pattern.substr(pos, 2) == "[:") {
            const auto close = pattern.find(":]", pos + 2);
            if (close == std::string_view::npos)
                throw std::runtime_error("unterminated POSIX character class");
            set = characterClass(pattern.substr(pos + 2, close - pos - 2));
            pos = close + 2;
            return -1;
        }
        if (pattern.substr(pos, 2) == "[." || pattern.substr(pos, 2) == "[=")
            throw std::runtime_error("collating elements and equivalence classes are not supported");
        auto c = take();
        if (c == '\\') {
            c = take();
            return shorthand(c, set) ? -1 : escaped(c);
        }
        return c;
    }
    Bytes bracket() {
        const bool negated = accept('^');
        Bytes set;
        bool first = true;
        while (first || peek() != ']') {
            first = false;
            Bytes subset;
            const auto lo = bracketAtom(subset);
            if (lo < 0) {
                set |= subset;
                continue;
            }
            if (lo != '-' && peek() == '-' && pos + 1 < pattern.size() && pattern[pos + 1] != ']') {
                ++pos;
                const auto hi = bracketAtom(subset);
                if (hi < 0)
                    throw std::runtime_error("character range endpoint must be a single byte");
                if (hi < lo)
                    throw std::runtime_error("descending character range");
                for (auto c = lo; c <= hi; ++c)
                    set.set(c);
            } else {
                set.set(lo);
            }
        }
        ++pos;
        return negated ? ~set : set;
    }
    Node atom(unsigned depth, bool allowBegin) {
        const auto c = take();
        if (c == '(') {
            if (depth == 128)
                throw std::runtime_error("regular expression nesting limit exceeded");
            auto node = alternation(depth + 1);
            if (!accept(')'))
                throw std::runtime_error("missing ')'");
            return node;
        }
        if ((c == '^' && allowBegin) ||
            (c == '$' && (peek() < 0 || peek() == ')' || peek() == '|')))
            return std::make_unique<Regex>(c == '^' ? Regex::begin : Regex::end);
        if (c == '*' || c == '+' || c == '?')
            throw std::runtime_error("repetition without an operand");
        auto node = std::make_unique<Regex>(Regex::bytes);
        if (c == '.') {
            node->set.set();
            node->set.reset('\n'); // Match the legacy scanner's dot behavior.
        } else if (c == '[') {
            node->set = bracket();
        } else if (c == '\\') {
            const auto e = take();
            if (e == 'b' || e == 'B')
                return std::make_unique<Regex>(e == 'b' ? Regex::boundary : Regex::notBoundary);
            if (!shorthand(e, node->set))
                node->set.set(escaped(e));
        } else {
            node->set.set(c);
        }
        return node;
    }
    unsigned number() {
        unsigned n = 0;
        if (!digit(peek()))
            throw std::runtime_error("missing repetition bound");
        while (digit(peek())) {
            n = n * 10 + (take() - '0');
            if (n >= unlimited)
                throw std::runtime_error("regular expression repetition limit exceeded");
        }
        return n;
    }
    bool interval() const {
        if (peek() != '{')
            return false;
        auto i = pos + 1;
        if (i == pattern.size() || !digit(pattern[i]))
            return false;
        while (i < pattern.size() && digit(pattern[i]))
            ++i;
        if (i < pattern.size() && pattern[i] == ',') {
            ++i;
            while (i < pattern.size() && digit(pattern[i]))
                ++i;
        }
        return i < pattern.size() && pattern[i] == '}';
    }
    Node repetition(unsigned depth, bool allowBegin) {
        auto child = atom(depth, allowBegin);
        for (unsigned nested = 0; peek() == '*' || peek() == '+' || peek() == '?' || interval(); ++nested) {
            if (nested == 8)
                throw std::runtime_error("regular expression repetition nesting limit exceeded");
            auto node = std::make_unique<Regex>(Regex::repeat);
            switch (take()) {
            case '*': node->max = unlimited; break;
            case '+': node->min = 1; node->max = unlimited; break;
            case '?': node->max = 1; break;
            default:
                node->min = node->max = number();
                if (accept(','))
                    node->max = peek() == '}' ? unlimited : number();
                if (!accept('}') || node->max < node->min)
                    throw std::runtime_error("invalid repetition bounds");
                break;
            }
            // Legacy Aurocks accepts lazy suffixes, but scanner selection is
            // always longest-prefix.
            accept('?');
            node->children.push_back(std::move(child));
            child = std::move(node);
        }
        return child;
    }
    Node sequence(unsigned depth) {
        auto node = std::make_unique<Regex>(Regex::concat);
        while (peek() >= 0 && peek() != ')' && peek() != '|') {
            node->children.push_back(repetition(depth, node->children.empty()));
        }
        return node;
    }
    Node alternation(unsigned depth) {
        auto node = std::make_unique<Regex>(Regex::alternate);
        node->children.push_back(sequence(depth));
        while (accept('|'))
            node->children.push_back(sequence(depth));
        return node;
    }

public:
    explicit RegexReader(std::string_view p) : pattern(p) {}

    Node read() {
        auto node = alternation(0);
        if (peek() >= 0)
            throw std::runtime_error("unexpected ')'");
        return node;
    }

    static bool unbounded(const Regex& node) { return node.max == unlimited; }
};

struct RegexState {
    enum Kind { match, bytes, split, begin, end, boundary, notBoundary } kind;
    int next = 0, other = 0;
};

// Emit a Thompson NFA in continuation form. State zero is the shared final
// state; a consuming state carries an index into the byte-set table.
class RegexProgram {
    std::size_t compileSteps = 0;

    int state(RegexState::Kind kind, int next = 0, int other = 0) {
        if (states.size() >= 65536)
            throw std::runtime_error("regular expression state limit exceeded");
        states.push_back({kind, next, other});
        return static_cast<int>(states.size() - 1);
    }
    int compile(const Regex& node, int next) {
        if (++compileSteps > 262144)
            throw std::runtime_error("regular expression compilation work limit exceeded");
        switch (node.kind) {
        case Regex::bytes:
            sets.push_back(node.set);
            return state(RegexState::bytes, next, static_cast<int>(sets.size() - 1));
        case Regex::begin: return state(RegexState::begin, next);
        case Regex::end: return state(RegexState::end, next);
        case Regex::boundary: return state(RegexState::boundary, next);
        case Regex::notBoundary: return state(RegexState::notBoundary, next);
        case Regex::concat:
            for (auto i = node.children.rbegin(); i != node.children.rend(); ++i)
                next = compile(**i, next);
            return next;
        case Regex::alternate: {
            int first = compile(*node.children.front(), next);
            for (std::size_t i = 1; i < node.children.size(); ++i) {
                const int alternative = compile(*node.children[i], next);
                first = state(RegexState::split, first, alternative);
            }
            return first;
        }
        case Regex::repeat: {
            const auto& child = *node.children.front();
            if (RegexReader::unbounded(node)) {
                const auto loop = state(RegexState::split, next);
                const auto body = compile(child, loop);
                states[loop].other = body;
                next = loop;
            } else {
                // Each optional iteration may jump straight to the continuation.
                const auto exit = next;
                for (auto i = node.min; i < node.max; ++i)
                    next = state(RegexState::split, compile(child, next), exit);
            }
            for (unsigned i = 0; i < node.min; ++i)
                next = compile(child, next);
            return next;
        }
        }
        return next;
    }

public:
    std::vector<RegexState> states{{RegexState::match}};
    std::vector<Bytes> sets;

    int add(std::string_view pattern) {
        return compile(*RegexReader(pattern).read(), 0);
    }
};

} // namespace aurocks::detail::portable
