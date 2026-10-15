module gateml.Compiler;
import gateml.AST;
import gateml.Module;
import gateml.Sema;
import gateml.Specs : identifier;
import std.utf : validateUTF8 = validate, UTFException;

/// Recursive-descent reader for the GateML v0.1 definition language.
Frontend compile(string source) {
    // NUL is checked first so definitions with embedded NULs keep reporting the
    // same failure as the C++ bootstrap regardless of later byte content.
    foreach (i, char c; source) if (c == '\0') throw new GateError("NUL byte in GateML definition", location(source, i, i + 1));
    try { validateUTF8(source); }
    catch (UTFException error) {
        throw new GateError("GateML definition must be UTF-8", location(source, error.idx, error.idx + 1));
    }
    struct Reader {
        string source;
        size_t offset;
        size_t line = 1, column = 1;
        // Positions are tracked incrementally: recomputing them from the start
        // of the source for every token would make reading quadratic.
        Span at() { return Span(offset, offset < source.length ? offset + 1 : offset, line, column); }
        private void advance(size_t count) {
            while (count-- && offset < source.length) {
                if (source[offset] == '\n') { ++line; column = 1; } else ++column;
                ++offset;
            }
        }
        void fail(string message) { throw new GateError(message, at()); }
        void trivia() {
            while (offset < source.length) {
                auto c = source[offset];
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(1); continue; }
                if (c == '/' && source.length - offset >= 2 && source[offset + 1] == '/') {
                    while (offset < source.length && source[offset] != '\n') ++offset;
                    if (offset < source.length) advance(1);
                    continue;
                }
                break;
            }
        }
        bool accept(string text) {
            trivia();
            if (source.length - offset < text.length || source[offset .. offset + text.length] != text) return false;
            advance(text.length); return true;
        }
        void expect(string text) { if (!accept(text)) fail("expected " ~ text); }
        string word(bool declaration = true) {
            trivia(); auto begin = offset, beginLine = line, beginColumn = column;
            while (offset < source.length) {
                auto c = source[offset];
                if (!(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c == '_' || c >= '0' && c <= '9')) break;
                ++offset;
            }
            auto value = source[begin .. offset];
            column += value.length; // identifiers cannot span lines
            if (!value.length || declaration && !identifier(value))
                throw new GateError("expected identifier", Span(begin, offset, beginLine, beginColumn));
            return value;
        }
        string quoted() {
            expect("\""); char[] result;
            while (offset < source.length) {
                auto begin = offset, beginLine = line, beginColumn = column;
                char c = source[offset];
                advance(1);
                if (c == '"') return result.idup;
                if (c == '\n' || c == '\r' || c == '\0')
                    throw new GateError("invalid string literal", Span(begin, offset, beginLine, beginColumn));
                if (c == '\\') {
                    if (offset == source.length) fail("unfinished string escape");
                    auto escapedAt = offset, escapedLine = line, escapedColumn = column;
                    c = source[offset];
                    advance(1);
                    switch (c) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case '\\', '"': break;
                        default: throw new GateError("unsupported string escape", Span(escapedAt, offset, escapedLine, escapedColumn));
                    }
                }
                result ~= c;
            }
            fail("unterminated string"); return null;
        }
    }
    Reader reader = Reader(source);
    if (reader.word(false) != "frontend") reader.fail("expected frontend header");
    Frontend result; result.name = reader.word(); reader.expect(";");
    bool hasStart;
    while (true) {
        reader.trivia(); if (reader.offset == source.length) break;
        auto section = reader.word(false);
        if (section != "lexer" && section != "parser" && section != "ast" && section != "adapter" && section != "profile")
            reader.fail("unknown section: " ~ section);
        reader.expect("{");
        while (!reader.accept("}")) {
            auto span = reader.at(); auto command = reader.word(false);
            switch (section) {
                case "lexer":
                    if (command != "token" && command != "skip") reader.fail("expected token or skip");
                    auto name = reader.word(); reader.expect("="); auto pattern = reader.quoted(); reader.expect(";");
                    result.lexer ~= LexRule(name, pattern, command == "skip", span); break;
                case "parser":
                    if (command == "start") {
                        if (hasStart) throw new GateError("duplicate start declaration", span);
                        result.start = reader.word(); reader.expect(";"); hasStart = true;
                    } else if (command == "rule") {
                        auto head = reader.word(); reader.expect("=");
                        do {
                            string[] body;
                            while (!reader.accept("=>")) body ~= reader.word();
                            auto node = reader.word(); result.productions ~= Production(head, body, node, span);
                        } while (reader.accept("|"));
                        reader.expect(";");
                    } else reader.fail("expected start or rule");
                    break;
                case "ast":
                    if (command != "node") reader.fail("expected node");
                    result.nodes ~= reader.word(); reader.expect(";"); break;
                case "adapter":
                    if (command != "lower") reader.fail("expected lower");
                    auto node = reader.word(); reader.expect("="); auto hook = reader.word(); reader.expect(";");
                    if (node in result.lowering) throw new GateError("duplicate lowering: " ~ node, span);
                    result.lowering[node] = hook; break;
                case "profile":
                    if (command != "option") reader.fail("expected option");
                    auto key = reader.word(); reader.expect("="); auto value = reader.quoted(); reader.expect(";");
                    if (key in result.options) throw new GateError("duplicate option: " ~ key, span);
                    result.options[key] = value; break;
                default: reader.fail("unknown section: " ~ section);
            }
        }
    }
    validate(result); return result;
}
