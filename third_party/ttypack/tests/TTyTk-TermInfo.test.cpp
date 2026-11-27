#include <TTyTk/TTyTk-TermInfo.hpp>

#include <cassert>

int main() {
    std::string bytes{};
    bytes.append("\x1a\x01\x04\x00\x01\x00\x01\x00\x01\x00\x03\x00", 12);
    bytes.append("x|x\0", 4);
    bytes.push_back('\1');
    bytes.push_back('\0');  // alignment after the one-byte Boolean table
    bytes.append("\x00\x00", 2); // one numeric capability
    bytes.append("\x00\x00", 2); // offset zero for the string capability
    bytes.append("ok\0", 3);
    const auto entry = ttytk::terminfo::load_from(bytes);
    assert(entry);
    assert(entry.value().flag(0));
    assert(entry.value().num(0) == 0);
    assert(entry.value().str(0) == "ok");
    assert(ttytk::terminfo::tparm("\x1b[%i%p1%d;%p2%dH", {0, 0}) == "\x1b[1;1H");
    assert(entry.value().num("cols") == 0);
    assert(entry.value().flag("bw"));
    assert(entry.value().str("cbt") == "ok");
    auto malformed = bytes;
    malformed[15] = 'a';
    assert(!ttytk::terminfo::load_from(malformed));
    malformed = bytes;
    malformed[20] = '\x10';
    assert(!ttytk::terminfo::load_from(malformed));

    std::string extended("\x1e\x02\x02\0\0\0\x01\0\0\0\0\0x\0\x00\x01\0\0", 18);
    // Tc flag, U8 number, Smulx string, and a canceled string.
    extended.append("\x01\0\x01\0\x02\0\x05\0\x14\0\x01\0", 12);
    extended.append("\x01\x02\x03\x04\x00\x00\xfe\xff\x00\x00\x03\x00\x06\x00\x0c\x00", 16);
    extended.append("yes\0Tc\0U8\0Smulx\0Gone\0", 21);
    // The extended table size is expressed in bytes, including all NULs.
    extended[26] = 21;
    const auto ext = ttytk::terminfo::load_from(extended);
    assert(ext);
    assert(ext.value().num("cols") == 256);
    assert(ext.value().flag("Tc"));
    assert(ext.value().num("U8") == 0x04030201);
    assert(ext.value().str("Smulx") == "yes");
    assert(!ext.value().str("Gone"));
    extended.pop_back();
    assert(!ttytk::terminfo::load_from(extended));

    using ttytk::terminfo::tparm;
    constexpr auto conditional = "%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d%e38;5;%p1%d%;";
    assert(tparm(conditional, 4) == "34");
    assert(tparm(conditional, 12) == "94");
    assert(tparm(conditional, 30) == "38;5;30");
    assert(tparm("%?%p1%t%?%p2%ta%eb%;%ec%;", 1, 0) == "b");
    assert(tparm("%?%p1%t%?%p2%ta%eb%;%ec%;", 0, 1) == "c");
    assert(tparm("%p1%Pa%ga%02X:%p2%l%d:%p2%.3s", 10, "word") == "0A:4:wor");
    assert(tparm("%p3%d%p1%d%", 7) == "07%");
}
