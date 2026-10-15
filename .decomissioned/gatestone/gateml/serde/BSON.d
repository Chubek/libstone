module gateml.serde.BSON;
import gateml.serde.JSON;
import gateml.serde.Binary;
import gateml.AST;
import std.algorithm : sort;
import std.conv : to;
import std.math : isFinite;

/// BSON documents for JSON objects. BSON has no unsigned 64-bit integer;
/// unsigned values above long.max fail explicitly rather than lose precision.
ubyte[] encode(const JSONValue value) {
    ubyte[] document(const JSONValue v, bool array, size_t depth) {
        checkDepth(depth); Writer payload;
        void element(string key, const JSONValue child) {
            checkText(key);
            foreach (char c; key) if (!c) throw new GateError("NUL in BSON key");
            ubyte tag;
            switch (child.type) {
                case JSONType.float_: tag = 1; break;
                case JSONType.string: tag = 2; break;
                case JSONType.object: tag = 3; break;
                case JSONType.array: tag = 4; break;
                case JSONType.true_, JSONType.false_: tag = 8; break;
                case JSONType.null_: tag = 10; break;
                case JSONType.integer, JSONType.uinteger: tag = 18; break;
                default: throw new GateError("unsupported BSON value");
            }
            payload.octet(tag); payload.raw(cast(const ubyte[]) key); payload.octet(0);
            switch (tag) {
                case 1:
                    if (!isFinite(child.floating)) throw new GateError("non-finite BSON float");
                    payload.le(doubleBits(child.floating), 8); break;
                case 2:
                    checkText(child.str);
                    if (child.str.length >= int.max) throw new GateError("BSON string too large");
                    payload.le(child.str.length + 1, 4); payload.raw(cast(const ubyte[]) child.str); payload.octet(0); break;
                case 3, 4: payload.raw(document(child, tag == 4, depth + 1)); break;
                case 8: payload.octet(child.type == JSONType.true_ ? 1 : 0); break;
                case 10: break;
                case 18:
                    if (child.type == JSONType.uinteger && child.uinteger > long.max) throw new GateError("BSON integer outside i64");
                    payload.le(child.type == JSONType.uinteger ? child.uinteger : cast(ulong) child.integer, 8); break;
                default: assert(0);
            }
        }
        if (array) foreach (i, child; v.array) element(i.to!string, child);
        else {
            if (v.type != JSONType.object) throw new GateError("BSON root must be an object");
            auto keys = v.object.keys.dup; sort(keys); foreach (key; keys) element(key, v.object[key]);
        }
        if (payload.data.length > int.max - 5) throw new GateError("BSON document too large");
        Writer result; result.le(payload.data.length + 5, 4); result.raw(payload.data); result.octet(0); return result.data;
    }
    return document(value, false, 0);
}

JSONValue decode(const(ubyte)[] bytes) {
    Reader input = Reader(bytes);
    JSONValue document(ref Reader outer, bool array, size_t depth) {
        checkDepth(depth); auto size = outer.le(4);
        if (size < 5 || size - 4 > outer.remaining || size > int.max) throw new GateError("invalid BSON document length");
        Reader reader = Reader(outer.take(cast(size_t) size - 4));
        auto result = array ? arrayValue() : objectValue(); size_t index;
        while (reader.remaining > 1) {
            auto tag = reader.octet();
            size_t start = reader.offset;
            while (reader.remaining && reader.data[reader.offset]) ++reader.offset;
            if (!reader.remaining) throw new GateError("unterminated BSON key");
            Reader keyReader = Reader(reader.data[start .. reader.offset]);
            auto key = keyReader.text(keyReader.remaining); reader.octet();
            JSONValue value;
            switch (tag) {
                case 1:
                    auto number = bitsDouble(reader.le(8));
                    if (!isFinite(number)) throw new GateError("non-finite BSON float");
                    value = JSONValue(number); break;
                case 2:
                    auto length = reader.le(4);
                    if (!length || length > reader.remaining) throw new GateError("invalid BSON string length");
                    value = JSONValue(reader.text(cast(size_t) length - 1));
                    if (reader.octet()) throw new GateError("unterminated BSON string");
                    break;
                case 3, 4: value = document(reader, tag == 4, depth + 1); break;
                case 8:
                    auto boolean = reader.octet(); if (boolean > 1) throw new GateError("invalid BSON boolean");
                    value = JSONValue(boolean == 1); break;
                case 10: value = JSONValue(null); break;
                case 16: value = JSONValue(cast(long) cast(int) reader.le(4)); break;
                case 18: value = JSONValue(cast(long) reader.le(8)); break;
                default: throw new GateError("unsupported BSON type");
            }
            if (array) {
                if (key != index.to!string) throw new GateError("non-contiguous BSON array");
                result.array ~= value; ++index;
            } else {
                if (key in result.object) throw new GateError("duplicate BSON key");
                result[key] = value;
            }
        }
        if (reader.octet()) throw new GateError("missing BSON terminator");
        reader.finish(); return result;
    }
    auto result = document(input, false, 0); input.finish(); return result;
}
