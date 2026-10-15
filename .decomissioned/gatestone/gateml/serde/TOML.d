module gateml.serde.TOML;
import gateml.serde.JSON;
import gateml.AST;
import gateml.serde.Binary;
import std.string : strip, replace;

/// Lossless GateML TOML envelope, not a general TOML parser. A JSON string
/// preserves nulls, mixed arrays and arbitrary keys absent from TOML's model.
string encode(const JSONValue value) {
    auto payload = gateml.serde.JSON.encode(JSONValue(gateml.serde.JSON.encode(value)));
    // JSON permits literal DEL; TOML basic strings require it to be escaped.
    payload = payload.replace("\x7f", "\\u007f");
    Writer writer;
    writer.raw(cast(const ubyte[]) "gateml_json = ");
    writer.raw(cast(const ubyte[]) payload); writer.octet('\n');
    return cast(string) writer.data.idup;
}
JSONValue decode(string text) {
    checkText(text);
    foreach (char c; text) if (c == '\x7f') throw new GateError("unescaped DEL in TOML envelope");
    text = text.strip;
    enum prefix = "gateml_json";
    if (text.length < prefix.length || text[0 .. prefix.length] != prefix) throw new GateError("expected GateML TOML envelope");
    text = text[prefix.length .. $].strip;
    if (!text.length || text[0] != '=') throw new GateError("expected TOML assignment");
    auto payload = gateml.serde.JSON.decode(text[1 .. $].strip);
    if (payload.type != JSONType.string) throw new GateError("TOML envelope payload must be a string");
    return gateml.serde.JSON.decode(payload.str);
}
