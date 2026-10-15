module gateml.serde.SExpression;
import gateml.serde.JSON;
import gateml.serde.Binary;
import gateml.AST;
import std.algorithm : sort;
import std.conv : to, ConvException;

/// Tagged S-expressions for JSON values, with JSON-escaped strings.
string encode(const JSONValue value, size_t depth = 0) {
    Writer writer;
    void raw(string text) { writer.raw(cast(const ubyte[]) text); }
    void emit(const JSONValue node, size_t nesting) {
        checkDepth(nesting);
        switch (node.type) {
            case JSONType.object:
                auto keys = node.object.keys.dup; sort(keys); raw("(object");
                foreach (key; keys) {
                    raw(" ("); raw(gateml.serde.JSON.encode(JSONValue(key))); raw(" ");
                    emit(node.object[key], nesting + 1); raw(")");
                }
                raw(")"); break;
            case JSONType.array:
                raw("(array"); foreach (child; node.array) { raw(" "); emit(child, nesting + 1); } raw(")"); break;
            case JSONType.integer: raw("(int " ~ node.integer.to!string ~ ")"); break;
            case JSONType.uinteger: raw("(uint " ~ node.uinteger.to!string ~ ")"); break;
            case JSONType.float_: raw("(real " ~ gateml.serde.JSON.encode(node) ~ ")"); break;
            default: raw(gateml.serde.JSON.encode(node)); break;
        }
    }
    emit(value, depth); return cast(string) writer.data.idup;
}
JSONValue decode(string text) {
    if (text.length > maxBytes) throw new GateError("S-expression size limit exceeded");
    size_t offset;
    void trivia() { while (offset < text.length && (text[offset] == ' ' || text[offset] == '\n' || text[offset] == '\r' || text[offset] == '\t')) ++offset; }
    bool accept(char c) { trivia(); if (offset < text.length && text[offset] == c) { ++offset; return true; } return false; }
    void expect(char c) { if (!accept(c)) throw new GateError("invalid S-expression delimiter"); }
    string atom() {
        trivia(); auto begin = offset;
        while (offset < text.length && text[offset] != ' ' && text[offset] != '\n' && text[offset] != '\r' && text[offset] != '\t' && text[offset] != '(' && text[offset] != ')') ++offset;
        if (begin == offset) throw new GateError("expected S-expression atom");
        return text[begin .. offset];
    }
    JSONValue read(size_t depth) {
        checkDepth(depth); trivia();
        if (offset == text.length) throw new GateError("truncated S-expression");
        if (text[offset] == '"') {
            auto begin = offset++; bool escaped;
            while (offset < text.length) {
                auto c = text[offset++];
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') return gateml.serde.JSON.decode(text[begin .. offset]);
            }
            throw new GateError("unterminated S-expression string");
        }
        if (!accept('(')) {
            auto value = atom();
            if (value != "null" && value != "true" && value != "false") throw new GateError("invalid S-expression scalar");
            return gateml.serde.JSON.decode(value);
        }
        auto tag = atom();
        if (tag == "object") {
            auto object = objectValue();
            while (!accept(')')) {
                expect('('); auto key = read(depth + 1);
                if (key.type != JSONType.string || key.str in object.object) throw new GateError("invalid S-expression key");
                object[key.str] = read(depth + 1); expect(')');
            }
            return object;
        }
        if (tag == "array") {
            auto array = arrayValue(); while (!accept(')')) array.array ~= read(depth + 1); return array;
        }
        auto number = atom(); expect(')');
        try {
            if (tag == "int") return JSONValue(number.to!long);
            if (tag == "uint") return JSONValue(number.to!ulong);
            if (tag == "real") {
                auto value = gateml.serde.JSON.decode(number);
                if (value.type == JSONType.float_) return value;
                if (value.type == JSONType.integer) return JSONValue(cast(double) value.integer);
                if (value.type == JSONType.uinteger) return JSONValue(cast(double) value.uinteger);
            }
        } catch (ConvException error) { throw new GateError("invalid S-expression number"); }
        throw new GateError("unknown S-expression tag: " ~ tag);
    }
    auto value = read(0); trivia();
    if (offset != text.length) throw new GateError("trailing S-expression input");
    return value;
}
