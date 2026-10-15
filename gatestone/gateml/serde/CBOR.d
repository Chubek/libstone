module gateml.serde.CBOR;
import gateml.serde.JSON;
import gateml.serde.Binary;
import gateml.AST;
import std.algorithm : sort;
import std.math : isFinite;

/// Definite-length CBOR for the JSON data model (RFC 8949). Tags, byte strings,
/// indefinite lengths and non-string map keys are deliberately rejected.
ubyte[] encode(const JSONValue value) {
    Writer writer;
    void head(ubyte major, ulong length) {
        if (length < 24) writer.octet(cast(ubyte)(major << 5 | length));
        else {
            ubyte extra; size_t width;
            if (length <= ubyte.max) { extra = 24; width = 1; }
            else if (length <= ushort.max) { extra = 25; width = 2; }
            else if (length <= uint.max) { extra = 26; width = 4; }
            else { extra = 27; width = 8; }
            writer.octet(cast(ubyte)(major << 5 | extra)); writer.be(length, width);
        }
    }
    void emit(const JSONValue v, size_t depth) {
        checkDepth(depth);
        switch (v.type) {
            case JSONType.null_: writer.octet(0xf6); break;
            case JSONType.true_: writer.octet(0xf5); break;
            case JSONType.false_: writer.octet(0xf4); break;
            case JSONType.integer:
                if (v.integer >= 0) head(0, cast(ulong) v.integer);
                else head(1, cast(ulong)(-(v.integer + 1)));
                break;
            case JSONType.uinteger: head(0, v.uinteger); break;
            case JSONType.float_:
                if (!isFinite(v.floating)) throw new GateError("non-finite CBOR float");
                writer.octet(0xfb); writer.be(doubleBits(v.floating), 8); break;
            case JSONType.string:
                checkText(v.str); head(3, v.str.length); writer.raw(cast(const ubyte[]) v.str); break;
            case JSONType.array:
                head(4, v.array.length); foreach (child; v.array) emit(child, depth + 1); break;
            case JSONType.object:
                auto keys = v.object.keys.dup; sort(keys);
                head(5, keys.length);
                foreach (key; keys) { emit(JSONValue(key), depth + 1); emit(v.object[key], depth + 1); }
                break;
            default: throw new GateError("unsupported CBOR value");
        }
    }
    emit(value, 0); return writer.data;
}

JSONValue decode(const(ubyte)[] bytes) {
    Reader reader = Reader(bytes);
    JSONValue read(size_t depth) {
        checkDepth(depth); auto initial = reader.octet(); auto major = initial >> 5; auto extra = initial & 31;
        ulong size;
        if (extra < 24) size = extra;
        else if (extra <= 27) size = reader.be(cast(size_t) 1 << (extra - 24));
        else throw new GateError("unsupported CBOR additional information");
        switch (major) {
            case 0: return size <= long.max ? JSONValue(cast(long) size) : JSONValue(size);
            case 1:
                if (size > long.max) throw new GateError("CBOR negative integer outside i64");
                return JSONValue(-1 - cast(long) size);
            case 3:
                if (size > reader.remaining) throw new GateError("invalid CBOR string length");
                return JSONValue(reader.text(cast(size_t) size));
            case 4:
                if (size > reader.remaining) throw new GateError("invalid CBOR array length");
                auto array = arrayValue(); foreach (unused; 0 .. size) array.array ~= read(depth + 1); return array;
            case 5:
                if (size > reader.remaining / 2) throw new GateError("invalid CBOR map length");
                auto object = objectValue();
                foreach (unused; 0 .. size) {
                    auto key = read(depth + 1);
                    if (key.type != JSONType.string || key.str in object.object) throw new GateError("invalid or duplicate CBOR map key");
                    object[key.str] = read(depth + 1);
                }
                return object;
            case 7:
                if (extra == 20 || extra == 21) return JSONValue(extra == 21);
                if (extra == 22) return JSONValue(null);
                if (extra >= 25 && extra <= 27) {
                    auto value = extra == 25 ? bitsHalf(cast(ushort) size) :
                        extra == 26 ? bitsFloat(cast(uint) size) : bitsDouble(size);
                    if (!isFinite(value)) throw new GateError("non-finite CBOR float");
                    return JSONValue(value);
                }
                throw new GateError("unsupported CBOR simple value");
            default: throw new GateError("unsupported CBOR major type");
        }
    }
    auto result = read(0); reader.finish(); return result;
}
