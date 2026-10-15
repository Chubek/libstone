module gateml.serde.Binary;
import gateml.AST;
import std.utf : validate;

enum maxDepth = 128;
enum maxBytes = 64 * 1024 * 1024;

struct Writer {
    ubyte[] data;
    void octet(ubyte value) {
        if (data.length >= maxBytes) throw new GateError("serialized output exceeds size limit");
        data ~= value;
    }
    void raw(const(ubyte)[] bytes) {
        if (data.length > maxBytes || bytes.length > maxBytes - data.length)
            throw new GateError("serialized output exceeds size limit");
        data ~= bytes;
    }
    void le(ulong value, size_t width) {
        checkWidth(width); foreach (i; 0 .. width) octet(cast(ubyte)(value >> (i * 8)));
    }
    void be(ulong value, size_t width) {
        checkWidth(width); foreach_reverse (i; 0 .. width) octet(cast(ubyte)(value >> (i * 8)));
    }
    void string32(string value) {
        checkText(value);
        if (value.length > uint.max) throw new GateError("string too long");
        le(value.length, 4); raw(cast(const ubyte[]) value);
    }
}

struct Reader {
    const(ubyte)[] data;
    size_t offset;
    this(const(ubyte)[] bytes) {
        if (bytes.length > maxBytes) throw new GateError("serialized input exceeds size limit");
        data = bytes;
    }
    size_t remaining() const {
        if (offset > data.length) throw new GateError("invalid serialized input offset");
        return data.length - offset;
    }
    const(ubyte)[] take(size_t count) {
        if (count > remaining) throw new GateError("truncated serialized input");
        auto result = data[offset .. offset + count]; offset += count; return result;
    }
    ubyte octet() { return take(1)[0]; }
    ulong le(size_t width) {
        checkWidth(width);
        ulong value; foreach (i, b; take(width)) value |= cast(ulong) b << (i * 8); return value;
    }
    ulong be(size_t width) {
        checkWidth(width); ulong value; foreach (b; take(width)) value = value << 8 | b; return value;
    }
    string text(size_t count) {
        auto value = cast(string) take(count).idup;
        checkText(value);
        return value;
    }
    string string32() { return text(cast(size_t) le(4)); }
    void finish() const { if (remaining) throw new GateError("trailing serialized input"); }
}

void checkDepth(size_t depth) { if (depth > maxDepth) throw new GateError("serialization nesting limit exceeded"); }
private void checkWidth(size_t width) {
    if (!width || width > 8) throw new GateError("integer width must be between 1 and 8 bytes");
}
void checkText(string text) {
    if (text.length > maxBytes) throw new GateError("serialized string exceeds size limit");
    try { validate(text); } catch (Exception error) { throw new GateError("invalid UTF-8 string"); }
}

private union DoubleBits { double value; ulong bits; }
ulong doubleBits(double value) { DoubleBits bits; bits.value = value; return bits.bits; }
double bitsDouble(ulong value) { DoubleBits bits; bits.bits = value; return bits.value; }

private union FloatBits { float value; uint bits; }
double bitsFloat(uint value) { FloatBits bits; bits.bits = value; return bits.value; }
double bitsHalf(ushort value) {
    import std.math : ldexp;
    auto exponent = (value >> 10) & 31;
    auto fraction = value & 1023;
    double result;
    if (exponent == 0) result = ldexp(cast(double) fraction, -24);
    else if (exponent == 31) result = fraction ? double.nan : double.infinity;
    else result = ldexp(cast(double)(fraction + 1024), cast(int) exponent - 25);
    return value & 0x8000 ? -result : result;
}
