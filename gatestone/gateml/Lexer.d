module gateml.Lexer;
import gateml.AST;
import gateml.Module;
import std.regex;
import std.string : indexOf;
import std.utf : validate, UTFException;

struct Token { string name, text; char code; Span span; }
struct Lexed { Token[] tokens; string tape; Span eof; }

/// Standalone D lexer using Phobos regex (not libchomsky3's ERE dialect).
/// Full-prefix tests recover longest matches even for ordered alternatives.
class Lexer {
    private LexRule[] rules;
    private Regex!char[] patterns;
    private Regex!char[] fullPatterns;
    private bool[] alternate;
    private char[] codes;
    this(const LexRule[] rules) {
        this.rules = rules.dup;
        Frontend frontend; frontend.lexer = this.rules;
        codes = tokenCodes(frontend);
        foreach (rule; rules) {
            try {
                auto pattern = regex("^(?:" ~ rule.pattern ~ ")");
                if (!matchFirst("", pattern).empty) throw new GateError("nullable lexical rule: " ~ rule.name, rule.span);
                patterns ~= pattern;
                fullPatterns ~= regex("^(?:" ~ rule.pattern ~ ")$");
                alternate ~= rule.pattern.indexOf('|') >= 0 || rule.pattern.indexOf("*?") >= 0 ||
                    rule.pattern.indexOf("+?") >= 0 || rule.pattern.indexOf("??") >= 0 || rule.pattern.indexOf("}?") >= 0;
            } catch (RegexException error) {
                throw new GateError("invalid pattern for " ~ rule.name ~ ": " ~ error.msg, rule.span);
            } catch (UTFException error) {
                throw new GateError("invalid UTF-8 pattern for " ~ rule.name, rule.span);
            }
        }
    }
    Lexed scan(string source, size_t maxTokens = size_t.max, size_t maxBytes = 1_048_576) {
        if (source.length > maxBytes) throw new GateError("reference lexer source size limit exceeded");
        try { validate(source); }
        catch (UTFException error) {
            throw new GateError("D reference lexer requires UTF-8 input", location(source, error.idx, error.idx + 1));
        }
        Lexed result;
        char[] tape;
        size_t offset, line = 1, column = 1;
        while (offset < source.length) {
            size_t best, chosen;
            foreach (i, pattern; patterns) {
                auto matched = matchFirst(source[offset .. $], pattern);
                size_t length = matched.empty || matched.pre.length ? 0 : matched.hit.length;
                if (length && alternate[i]) {
                    for (size_t candidate = source.length - offset; candidate > length && candidate > best; --candidate) {
                        // Do not hand truncated UTF-8 sequences to Phobos regex.
                        if (offset + candidate < source.length && (cast(ubyte) source[offset + candidate] & 0xc0) == 0x80) continue;
                        auto full = matchFirst(source[offset .. offset + candidate], fullPatterns[i]);
                        if (!full.empty && !full.pre.length && full.hit.length == candidate) { length = candidate; break; }
                    }
                }
                if (length > best) { best = length; chosen = i; }
            }
            if (!best) throw new GateError("unrecognized input byte", Span(offset, offset + 1, line, column));
            auto text = source[offset .. offset + best];
            if (!rules[chosen].skip) {
                if (result.tokens.length >= maxTokens)
                    throw new GateError("reference parser token limit exceeded", Span(offset, offset + best, line, column));
                result.tokens ~= Token(rules[chosen].name, text, codes[chosen], Span(offset, offset + best, line, column));
                tape ~= codes[chosen];
            }
            foreach (char c; text) {
                if (c == '\n') { ++line; column = 1; } else ++column;
            }
            offset += best;
        }
        result.tape = tape.idup;
        result.eof = Span(offset, offset, line, column);
        return result;
    }
}
