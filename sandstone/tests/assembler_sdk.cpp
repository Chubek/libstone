#include "assembler.h"
#include "sdk.h"
#include "sandstone.hpp"
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

static void require(bool value, const std::string &message) { if (!value) throw std::runtime_error(message); }
int main() try {
    domsymas::Builder builder(true);
    domsymas::TargetRec target; target.arch = "test"; target.addr_size = 64;
    builder.add_target(target);
    domsymas::Databank bank; require(builder.build(bank).ok(), "bank build");
    std::vector<uint8_t> data; require(bank.to_bytes(data).ok(), "bank serialize");
    sandstone_asm_t *raw = nullptr; char error[2048] = {};
    require(!sandstone_asm_new_from_memory(&raw, data.data(), data.size(), error, sizeof error), error);
    std::unique_ptr<sandstone_asm_t, decltype(&sandstone_asm_free)> ctx(raw, sandstone_asm_free);
    auto assemble = [&](const char *source, std::vector<uint8_t> expected) {
        int status = sandstone_asm_assemble_string(raw, source, "sdk.s");
        require(!status, sandstone_asm_error(raw));
        size_t n = 0; const auto *bytes = sandstone_asm_image(raw, &n);
        require(n == expected.size() && (!n || std::memcmp(bytes, expected.data(), n) == 0), std::string("wrong bytes: ") + source);
    };
    auto reject = [&](const char *source, const char *diagnostic) {
        require(sandstone_asm_assemble_string(raw, source, "bad.s") != 0, std::string("unexpected success: ") + source);
        require(std::strstr(sandstone_asm_error(raw), diagnostic), sandstone_asm_error(raw));
    };
    assemble(".asciz \"a\", \"b\"\n", {'a', 0, 'b', 0});
    assemble(".set V, 1\n.byte V\n.set V, 2\n.byte V, 2 & 1 == 0\n", {1,2,0});
    assemble(".ascii \"; # quote: \\\"\" ; comment\n", {';', ' ', '#', ' ', 'q','u','o','t','e',':',' ', '"'});
    assemble(".ascii \"\\x41\\101\\n\"\n", {'A','A','\n'});
    assemble(".hword 0x1234\n.uleb128 624485\n.sleb128 -624485\n", {0x34,0x12,0xe5,0x8e,0x26,0x9b,0xf1,0x59});
    assemble(".byte 1\n.data\n.byte 2\n.balign 4\n.byte 3\n", {1,2,0,0,0,3});
    assemble(R"(.equ N, 2
.if N == 2
.macro pair a, b = 9
.byte \a, \b
.endm
pair 7
.elseif 1
.error wrong branch
.else
.error wrong branch
.endif
.if 0
.include "does-not-exist"
<: error('inactive') :>
.endif
.rept 2
.byte 3
.endr
)", {7,9,3,3});
    assemble(R"(.macro text value
.ascii \value
.endmacro
text "a,b"
.macro many values...
.byte \values
.endmacro
many 1,2,3
)", {'a',',','b',1,2,3});
    assemble(R"(.equ N, 3
<:
lsandas.injectf('.byte ~li', {1,2,N})
lsandas.register_directive('.twice', function(arg)
  lsandas.injectf('.byte %d, %d', tonumber(arg), tonumber(arg))
end)
:>
.twice 4
)", {1,2,3,4,4});
    assemble("<: assert(N == nil); lsandas.inject('.byte 5\\n') :>\n", {5});
    assemble(R"(<:
local marker = ':>'
local long = [==[:>]==]
-- :> in a comment
--[=[ :> in a long comment ]=]
assert(marker == long)
lsandas.inject('.byte 8\n')
:>
)", {8});
    assemble(R"(.struct Packet
.field tag, 1
.align 4
.field value, 4
.endstruct
.enum Kind
FIRST = 3
SECOND
.endenum
.byte Packet.value, Packet.size, Kind.SECOND
.macro labelled a, b
local label
label: .byte \1, \2
.endm
labelled 5, 6
labelled 7, 8
)", {4,8,4,5,6,7,8});
    require(!sandstone_asm_register_directive(raw, ".sdk", [](void *, const char *args, sandstone_source_sink emit, void *sink, char *, size_t) {
        std::string out = ".byte " + std::string(args) + "\n";
        return emit(sink, out.data(), out.size());
    }, nullptr), "register SDK directive");
    require(!sandstone_asm_define_macro(raw, "sdk_macro", "n = 6", ".sdk \\n"), "define SDK macro");
    assemble("sdk_macro\n.sdk 7\n", {6,7});
    require(sandstone_asm_register_directive(raw, ".byte", nullptr, nullptr) != 0, "reserved directive");
    require(!sandstone_asm_register_directive(raw, ".bad_sink", [](void *, const char *, sandstone_source_sink emit, void *sink, char *, size_t) {
        emit(sink, ".byte 1\0.byte 2", 15); return 0;
    }, nullptr), "register failing sink");
    reject(".bad_sink\n", "invalid or oversized");
    reject(".if missing\n.byte 1\n.endif\n", "invalid constant");
    reject(".if 1\n.else\n.else\n.endif\n", "unexpected .else");
    reject(".macro m a\n.byte \\a\n.endm\nm\n", "missing macro argument");
    reject("<: lsandas.injectf('.byte %d', {}) :>\n", "argument count");
    reject("<: lsandas.inject(false) :>\n", "inject expects");
    reject(".byte 256\n", "range");
    reject(".space 4294967296\n", "count");
    reject(".fill 2, 9, 0\n", "COUNT,SIZE");
    reject(".quad 9223372036854775807 + 1\n", "evaluate");
    reject(".equ bad, missing\n.byte 1\n", "undefined");
#ifdef ARCHML_PROFILE
    int loaded = sandstone_asm_load_archml(raw, ARCHML_PROFILE);
    require(!loaded, sandstone_asm_error(raw));
    assemble(".insn nop\n", {0x90});
    reject(".insn not_an_instruction\n", "unknown");
#endif
    std::cout << "assembler SDK/directive/Lua regressions passed\n";
    return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
