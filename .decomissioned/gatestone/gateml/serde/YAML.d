module gateml.serde.YAML;
import gateml.serde.JSON;
import gateml.serde.Binary;
import gateml.AST;
import std.string : strip;

/// YAML 1.2's JSON-compatible flow subset. No aliases, tags or implicit scalar
/// conversion; decode deliberately accepts only the subset emitted by encode.
string encode(const JSONValue value) {
    Writer writer; writer.raw(cast(const ubyte[]) "---\n");
    writer.raw(cast(const ubyte[]) gateml.serde.JSON.encode(value)); writer.octet('\n');
    return cast(string) writer.data.idup;
}
JSONValue decode(string text) {
    checkText(text);
    text = text.strip;
    if (text.length >= 3 && text[0 .. 3] == "---") {
        if (text.length > 3 && text[3] != ' ' && text[3] != '\t' && text[3] != '\r' && text[3] != '\n')
            throw new GateError("invalid YAML document marker");
        text = text[3 .. $].strip;
    }
    return gateml.serde.JSON.decode(text);
}
