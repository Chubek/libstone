module gateml.serde.MSGPack;
import gateml.serde.JSON;
import gateml.serde.Binary;
import gateml.AST;
import std.algorithm : sort;
import std.math : isFinite;

/// MessagePack JSON model, with deterministic keys and native 64-bit numbers.
ubyte[] encode(const JSONValue value) {
    Writer writer;
    void length(ubyte medium, ubyte large, size_t size) {
        if (size <= ushort.max) { writer.octet(medium); writer.be(size, 2); }
        else if (size <= uint.max) { writer.octet(large); writer.be(size, 4); }
        else throw new GateError("MessagePack collection too large");
    }
    void emit(const JSONValue v, size_t depth) {
        checkDepth(depth);
        switch (v.type) {
            case JSONType.null_: writer.octet(0xc0); break;
            case JSONType.false_: writer.octet(0xc2); break;
            case JSONType.true_: writer.octet(0xc3); break;
            case JSONType.integer:
                if (v.integer >= 0 && v.integer <= 127 || v.integer >= -32 && v.integer < 0) writer.octet(cast(ubyte) v.integer);
                else { writer.octet(0xd3); writer.be(cast(ulong) v.integer, 8); }
                break;
            case JSONType.uinteger: writer.octet(0xcf); writer.be(v.uinteger, 8); break;
            case JSONType.float_:
                if (!isFinite(v.floating)) throw new GateError("non-finite MessagePack float");
                writer.octet(0xcb); writer.be(doubleBits(v.floating), 8); break;
            case JSONType.string:
                checkText(v.str);
                if (v.str.length < 32) writer.octet(cast(ubyte)(0xa0 | v.str.length));
                else if (v.str.length <= 255) { writer.octet(0xd9); writer.octet(cast(ubyte) v.str.length); }
                else length(0xda, 0xdb, v.str.length);
                writer.raw(cast(const ubyte[]) v.str); break;
            case JSONType.array:
                if (v.array.length < 16) writer.octet(cast(ubyte)(0x90 | v.array.length));
                else length(0xdc, 0xdd, v.array.length);
                foreach (child; v.array) emit(child, depth + 1); break;
            case JSONType.object:
                auto keys = v.object.keys.dup; sort(keys);
                if (keys.length < 16) writer.octet(cast(ubyte)(0x80 | keys.length));
                else length(0xde, 0xdf, keys.length);
                foreach (key; keys) { emit(JSONValue(key), depth + 1); emit(v.object[key], depth + 1); }
                break;
            default: throw new GateError("unsupported MessagePack value");
        }
    }
    emit(value, 0); return writer.data;
}

JSONValue decode(const(ubyte)[] bytes) {
    Reader reader = Reader(bytes);
    JSONValue read(size_t depth) {
        checkDepth(depth); auto tag = reader.octet();
        if (tag < 0x80) return JSONValue(cast(long) tag);
        if (tag >= 0xe0) return JSONValue(cast(long) cast(byte) tag);
        ulong length; ubyte kind;
        if ((tag & 0xe0) == 0xa0) { kind = 1; length = tag & 31; }
        else if ((tag & 0xf0) == 0x90) { kind = 2; length = tag & 15; }
        else if ((tag & 0xf0) == 0x80) { kind = 3; length = tag & 15; }
        else switch (tag) {
            case 0xc0: return JSONValue(null);
            case 0xc2, 0xc3: return JSONValue(tag == 0xc3);
            case 0xcc, 0xcd, 0xce, 0xcf:
                auto number = reader.be(cast(size_t) 1 << (tag - 0xcc));
                return number <= long.max ? JSONValue(cast(long) number) : JSONValue(number);
            case 0xd0: return JSONValue(cast(long) cast(byte) reader.be(1));
            case 0xd1: return JSONValue(cast(long) cast(short) reader.be(2));
            case 0xd2: return JSONValue(cast(long) cast(int) reader.be(4));
            case 0xd3: return JSONValue(cast(long) reader.be(8));
            case 0xcb:
                auto number = bitsDouble(reader.be(8));
                if (!isFinite(number)) throw new GateError("non-finite MessagePack float");
                return JSONValue(number);
            case 0xca:
                union FloatBits { uint bits; float value; }
                FloatBits number; number.bits = cast(uint) reader.be(4);
                if (!isFinite(number.value)) throw new GateError("non-finite MessagePack float");
                return JSONValue(cast(double) number.value);
            case 0xd9: kind = 1; length = reader.be(1); break;
            case 0xda, 0xdb: kind = 1; length = reader.be(tag == 0xda ? 2 : 4); break;
            case 0xdc, 0xdd: kind = 2; length = reader.be(tag == 0xdc ? 2 : 4); break;
            case 0xde, 0xdf: kind = 3; length = reader.be(tag == 0xde ? 2 : 4); break;
            default: throw new GateError("unsupported MessagePack tag");
        }
        if (length > reader.remaining) throw new GateError("invalid MessagePack length");
        if (kind == 1) return JSONValue(reader.text(cast(size_t) length));
        if (kind == 2) {
            auto array = arrayValue(); foreach (unused; 0 .. length) array.array ~= read(depth + 1); return array;
        }
        if (length > reader.remaining / 2) throw new GateError("invalid MessagePack map length");
        auto object = objectValue();
        foreach (unused; 0 .. length) {
            auto key = read(depth + 1);
            if (key.type != JSONType.string || key.str in object.object) throw new GateError("invalid or duplicate MessagePack map key");
            object[key.str] = read(depth + 1);
        }
        return object;
    }
    auto result = read(0); reader.finish(); return result;
}
