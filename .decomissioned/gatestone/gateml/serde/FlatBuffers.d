module gateml.serde.FlatBuffers;
import gateml.serde.JSON;
import gateml.serde.Binary;
import gateml.AST;

/// Standard FlatBuffer with schema:
/// table Envelope { json:string (required); }
/// root_type Envelope; file_identifier "GMLF";
/// The JSON payload is canonical and versioned by the frontend descriptor.
ubyte[] encode(const JSONValue value) {
    auto payload = gateml.serde.JSON.encode(value);
    if (payload.length > uint.max - 29) throw new GateError("FlatBuffer payload too large");
    Writer writer;
    writer.le(16, 4); writer.raw(cast(const ubyte[]) "GMLF");
    writer.le(6, 2); writer.le(8, 2); writer.le(4, 2); writer.le(0, 2);
    writer.le(8, 4); writer.le(4, 4);
    writer.le(payload.length, 4); writer.raw(cast(const ubyte[]) payload); writer.octet(0);
    return writer.data;
}
JSONValue decode(const(ubyte)[] bytes) {
    Reader reader = Reader(bytes);
    auto root = reader.le(4);
    if (cast(string) reader.take(4) != "GMLF" || root < 8 || root % 4 || root > bytes.length || bytes.length - root < 4)
        throw new GateError("invalid FlatBuffer root");
    reader.offset = cast(size_t) root;
    auto distance = cast(int) reader.le(4);
    // Vtable deduplication can place a vtable after its table (negative offset).
    auto vtable = cast(long) root - distance;
    if (!distance || vtable < 8 || vtable % 2 || vtable > bytes.length || bytes.length - vtable < 6)
        throw new GateError("invalid FlatBuffer vtable offset");
    reader.offset = cast(size_t) vtable;
    auto vsize = reader.le(2); auto osize = reader.le(2);
    if (vsize < 6 || vsize % 2 || osize < 8 || vsize > bytes.length - vtable || osize > bytes.length - root)
        throw new GateError("invalid FlatBuffer table size");
    if (cast(ulong) vtable < root + osize && cast(ulong) vtable + vsize > root)
        throw new GateError("overlapping FlatBuffer table and vtable");
    auto field = reader.le(2);
    if (field < 4 || field % 4 || field > osize - 4) throw new GateError("missing FlatBuffer json field");
    reader.offset = cast(size_t)(root + field);
    auto relative = reader.le(4);
    if (relative < 4 || relative % 4 || relative > bytes.length - (root + field)) throw new GateError("invalid FlatBuffer string offset");
    reader.offset = cast(size_t)(root + field + relative);
    auto length = reader.le(4);
    if (length >= reader.remaining) throw new GateError("invalid FlatBuffer string length");
    auto payload = reader.text(cast(size_t) length);
    if (reader.octet()) throw new GateError("missing FlatBuffer string terminator");
    // FlatBuffers may contain alignment padding or other objects after the
    // referenced string. It is not a self-delimiting stream format.
    return gateml.serde.JSON.decode(payload);
}
