module gateml.serde.JSON;
public import std.json : JSONValue, JSONType;
import std.json : parseJSON, toJSON, JSONException, JSONOptions;
import std.algorithm : sort;
import std.math : isFinite;
import std.utf : codeLength;
import gateml.AST;
import gateml.Module;
import gateml.Specs : formatName, descriptorVersion;
import gateml.Sema;
import gateml.serde.Binary;

/// Canonical compact JSON with sorted object keys and source-ordered arrays.
string encode(const JSONValue value, size_t depth = 0) {
    Writer writer;
    void raw(string text) { writer.raw(cast(const ubyte[]) text); }
    void quoted(string text) {
        checkText(text); writer.octet('"');
        foreach (offset, dchar c; text) {
            switch (c) {
                case '"': raw("\\\""); break;
                case '\\': raw("\\\\"); break;
                case '\b': raw("\\b"); break;
                case '\f': raw("\\f"); break;
                case '\n': raw("\\n"); break;
                case '\r': raw("\\r"); break;
                case '\t': raw("\\t"); break;
                default:
                    if (c < 32 || c >= 0x7f && c <= 0x9f) {
                        enum hex = "0123456789abcdef";
                        raw("\\u00"); writer.octet(hex[c >> 4]); writer.octet(hex[c & 15]);
                    } else writer.raw(cast(const ubyte[]) text[offset .. offset + codeLength!char(c)]);
            }
        }
        writer.octet('"');
    }
    void emit(const JSONValue node, size_t nesting) {
        checkDepth(nesting);
        switch (node.type) {
            case JSONType.object:
                auto keys = node.object.keys.dup; sort(keys);
                writer.octet('{');
                foreach (i, key; keys) {
                    if (i) writer.octet(',');
                    quoted(key); writer.octet(':'); emit(node.object[key], nesting + 1);
                }
                writer.octet('}'); break;
            case JSONType.array:
                writer.octet('[');
                foreach (i, child; node.array) { if (i) writer.octet(','); emit(child, nesting + 1); }
                writer.octet(']'); break;
            case JSONType.float_:
                if (!isFinite(node.floating)) throw new GateError("non-finite JSON number");
                raw(toJSON(node)); break;
            case JSONType.string: quoted(node.str); break;
            default: raw(toJSON(node)); break;
        }
    }
    emit(value, depth);
    return cast(string) writer.data.idup;
}

JSONValue decode(string text) {
    if (text.length > maxBytes) throw new GateError("JSON size limit exceeded");
    checkText(text);
    // Bound nesting before invoking Phobos's recursive parser.
    size_t depth; bool quoted, escaped;
    foreach (char c; text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') checkDepth(++depth);
        else if ((c == ']' || c == '}') && depth) --depth;
    }
    try { auto value = parseJSON(text, JSONOptions.strictParsing); encode(value); return value; }
    catch (JSONException error) { throw new GateError("invalid JSON: " ~ error.msg); }
}

JSONValue objectValue() { return JSONValue(cast(JSONValue[string]) null); }
JSONValue arrayValue() { return JSONValue(cast(JSONValue[]) null); }

JSONValue frontendValue(const Frontend frontend) {
    validate(frontend);
    auto result = objectValue();
    result["format"] = JSONValue(formatName); result["version"] = JSONValue(descriptorVersion);
    result["name"] = JSONValue(frontend.name); result["start"] = JSONValue(frontend.start);
    result["value_type"] = JSONValue("i64");
    auto rules = arrayValue(); auto codes = tokenCodes(frontend);
    foreach (i, rule; frontend.lexer) {
        auto entry = objectValue(); entry["name"] = JSONValue(rule.name); entry["pattern"] = JSONValue(rule.pattern);
        entry["skip"] = JSONValue(rule.skip); entry["code"] = rule.skip ? JSONValue(null) : JSONValue([codes[i]].idup);
        rules.array ~= entry;
    }
    result["lexer"] = rules;
    auto productions = arrayValue();
    foreach (rule; frontend.productions) {
        auto entry = objectValue(); entry["head"] = JSONValue(rule.head); entry["node"] = JSONValue(rule.node);
        auto body = arrayValue(); foreach (symbol; rule.body) body.array ~= JSONValue(symbol);
        entry["body"] = body; productions.array ~= entry;
    }
    result["productions"] = productions;
    auto nodes = arrayValue(); foreach (node; frontend.nodes) nodes.array ~= JSONValue(node);
    result["nodes"] = nodes;
    auto lowering = objectValue(); foreach (key, value; frontend.lowering) lowering[key] = JSONValue(value);
    auto options = objectValue(); foreach (key, value; frontend.options) options[key] = JSONValue(value);
    result["lowering"] = lowering; result["options"] = options;
    return result;
}

string descriptor(const Frontend frontend) { return encode(frontendValue(frontend)); }

Frontend readDescriptor(const JSONValue value) {
    Frontend result;
    try {
        if (value["format"].str != formatName || value["version"].integer != descriptorVersion || value["value_type"].str != "i64")
            throw new GateError("unsupported frontend descriptor");
        result.name = value["name"].str; result.start = value["start"].str;
        foreach (rule; value["lexer"].array) {
            if (rule["skip"].type != JSONType.true_ && rule["skip"].type != JSONType.false_) throw new GateError("invalid skip flag");
            result.lexer ~= LexRule(rule["name"].str, rule["pattern"].str, rule["skip"].type == JSONType.true_);
        }
        auto codes = tokenCodes(result);
        foreach (i, rule; value["lexer"].array) {
            if (result.lexer[i].skip) { if (rule["code"].type != JSONType.null_) throw new GateError("invalid trivia code"); }
            else if (rule["code"].str != [codes[i]].idup) throw new GateError("invalid token code");
        }
        foreach (rule; value["productions"].array) {
            string[] body; foreach (symbol; rule["body"].array) body ~= symbol.str;
            result.productions ~= Production(rule["head"].str, body, rule["node"].str);
        }
        foreach (node; value["nodes"].array) result.nodes ~= node.str;
        foreach (key, hook; value["lowering"].object) result.lowering[key] = hook.str;
        foreach (key, option; value["options"].object) result.options[key] = option.str;
    } catch (JSONException error) { throw new GateError("malformed frontend descriptor: " ~ error.msg); }
    validate(result); return result;
}

JSONValue treeValue(Tree tree, size_t depth = 0) {
    checkDepth(depth);
    if (tree is null) throw new GateError("null tree");
    auto result = objectValue(); result["name"] = JSONValue(tree.name);
    result["start"] = JSONValue(cast(ulong) tree.span.begin); result["end"] = JSONValue(cast(ulong) tree.span.end);
    result["line"] = JSONValue(cast(ulong) tree.span.line); result["column"] = JSONValue(cast(ulong) tree.span.column);
    if (tree.terminal) result["text"] = JSONValue(tree.text);
    auto children = arrayValue(); foreach (child; tree.children) children.array ~= treeValue(child, depth + 1);
    result["children"] = children; return result;
}
