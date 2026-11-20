#include "assembler.h"
#include "sdk.h"
#include "sandstone.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

static void require(bool value, const std::string &message) { if (!value) throw std::runtime_error(message); }

static void check_asm_opt_evidence(const char *nop_source, bool expected, bool with_reloc = false) {
    domsymas::Builder builder(true);
    domsymas::TargetRec target;
    target.arch = "riscv64";
    target.addr_size = 64;
    const auto feature = builder.add_feature("i", 0, "test feature");
    target.features = {feature};
    builder.add_target(target);
    domsymas::SyntaxRec syntax;
    syntax.name = "default";
    syntax.separator = ", ";
    builder.add_syntax(syntax);
    domsymas::RegisterRec zero;
    zero.name = "x0";
    zero.width = 64;
    builder.add_register(zero);
    builder.add_reloc("TEST_RELOC", 32, 0, 0, 0, 0, DOMSYMAS_ADDEND_EXPLICIT, "S + A");

    const auto addi = builder.add_mnemonic("addi", {}, 0);
    const auto addi_encoder = builder.add_encoder(
        "addi", "emit32le(0x13 + rd*128 + rs1*32768 + imm*1048576);", 0, 4, 0);
    const auto addi_form = builder.add_form(addi, 3, 0, addi_encoder, {feature}, {}, {});
    for (uint32_t i = 0; i < 3; i++) {
        domsymas::OperandRec operand;
        operand.form = addi_form;
        operand.position = i;
        operand.kind = i == 2 ? 1 : 0; // IMM, REG
        operand.width = i == 2 ? 12 : 64;
        builder.add_operand(operand);
    }
    builder.link_mnemonic_form(addi, addi_form);
    uint32_t nop_form = 0;
    if (nop_source) {
        const auto nop = builder.add_mnemonic("nop", {}, 0);
        const auto encoder = builder.add_encoder("nop", nop_source, 0, 4, 1);
        nop_form = builder.add_form(nop, 0, 0, encoder, {}, {}, {});
        builder.link_mnemonic_form(nop, nop_form);
    }

    domsymas::Databank bank;
    require(builder.build(bank).ok(), "probe bank build");
    if (with_reloc) {
        domsymas::Databank::EncodeResult encoded;
        require(bank.encode(nop_form, {}, encoded).ok() && encoded.relocs.size() == 1 &&
                    encoded.bytes == std::vector<uint8_t>{0x13, 0, 0, 0},
                "expected a matching NOP encoding with a relocation");
    }
    std::vector<uint8_t> data;
    require(bank.to_bytes(data).ok(), "probe bank serialize");
    sandstone_asm_t *raw = nullptr;
    char error[2048] = {};
    require(!sandstone_asm_new_from_memory(&raw, data.data(), data.size(), error, sizeof error), error);
    std::unique_ptr<sandstone_asm_t, decltype(&sandstone_asm_free)> ctx(raw, sandstone_asm_free);
    constexpr std::string_view script = R"(
assert(asm_context.arch == "riscv64" and asm_context.syntax == "default")
function asm_optimize(source, name)
    assert(name == "<string>")
    return asm_context.riscv64_nop_alias and ".byte 1\n" or ".byte 0\n"
end
)";
    int status = sandstone_asm_set_asm_opt_hook(raw, script.data(), script.size());
    require(!status, sandstone_asm_error(raw));
    auto check = [&](bool value) {
        status = sandstone_asm_assemble_string(raw, ".byte 2\n", nullptr);
        require(!status, sandstone_asm_error(raw));
        size_t size = 0;
        const auto *bytes = sandstone_asm_image(raw, &size);
        require(size == 1 && bytes[0] == value, "incorrect RV64 alias evidence");
    };
    check(expected);
    if (expected) {
        require(!sandstone_asm_disable_feature(raw, "i"), "disable required RV64 feature");
        check(false);
        require(!sandstone_asm_enable_feature(raw, "i"), "restore required RV64 feature");
        check(true);
    }
}

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

    auto set_asm_opt = [&](std::string_view script) {
        int status = sandstone_asm_set_asm_opt_hook(raw, script.data(), script.size());
        require(!status, sandstone_asm_error(raw));
    };
    std::string optimizer = R"lua(
local called = false
function asm_optimize(source, name)
  assert(not called and name == "sdk.s")
  called = true
  assert(lsandas == nil and inline_marker == nil)
  assert(os == nil and io == nil and require == nil and debug == nil)
  assert(rawset == nil and getmetatable(asm_context) == false)
  assert(asm_context.arch == "test" and asm_context.syntax == nil)
  assert(asm_context.riscv64_nop_alias == false)
  local ok, err = pcall(function() asm_context.arch = "riscv64" end)
  assert(not ok and err:find("asm_context is read-only", 1, true))
  assert(not pcall(function() asm_context.riscv64_nop_alias = true end))
  assert(asm_context.arch == "test" and asm_context.riscv64_nop_alias == false)
  assert(not source:find("sdk_macro", 1, true))
  local text, count = source:gsub("%.byte 6\n", ".byte 61, 62\n")
  assert(count == 1)
  return text
end
)lua";
    set_asm_opt(optimizer);
    optimizer.assign(optimizer.size(), '!');
    const char *macro_source = "<: assert(io); inline_marker = true :>\nsdk_macro\n.sdk 7\n";
    assemble(macro_source, {61,62,7});
    assemble(macro_source, {61,62,7}); // No optimizer globals survive an invocation.
    auto unexpanded = std::unique_ptr<char, decltype(&std::free)>(
        sandstone_asm_preprocess(raw, macro_source), std::free);
    require(unexpanded && std::strcmp(unexpanded.get(), macro_source) == 0, "standalone preprocessing changed");

    sandstone_asm_t *other = nullptr;
    require(!sandstone_asm_new_from_memory(&other, data.data(), data.size(), error, sizeof error), error);
    std::unique_ptr<sandstone_asm_t, decltype(&sandstone_asm_free)> other_ctx(other, sandstone_asm_free);
    int status = sandstone_asm_assemble_string(other, ".byte 6\n", "sdk.s");
    require(!status, sandstone_asm_error(other));
    size_t other_size = 0;
    const auto *other_bytes = sandstone_asm_image(other, &other_size);
    require(other_size == 1 && other_bytes[0] == 6, "asm-opt leaked into a separate context");

    auto reject_script = [&](std::string_view script, const char *diagnostic) {
        require(sandstone_asm_set_asm_opt_hook(raw, script.data(), script.size()) != 0,
                "invalid asm-opt script accepted");
        require(std::strstr(sandstone_asm_error(raw), diagnostic), sandstone_asm_error(raw));
    };
    reject_script({}, "no Lua script supplied");
    const char nul_script[] = "-- before NUL\0-- after NUL";
    reject_script({nul_script, sizeof nul_script - 1}, "NUL in Lua script");
    reject_script("function asm_optimize(", "asm-opt");
    require(sandstone_asm_load_asm_opt_hook(raw, nullptr) != 0, "missing asm-opt path accepted");
    require(std::strstr(sandstone_asm_error(raw), "no Lua script path supplied"), sandstone_asm_error(raw));
    assemble(macro_source, {61,62,7}); // A failed replacement leaves the old hook.

    auto reject_asm_opt = [&](std::string_view script, const char *diagnostic) {
        set_asm_opt(script);
        reject(".byte 7\n", diagnostic);
        uint32_t symbols = 1, relocs = 1;
        size_t bytes = 1;
        require(!sandstone_asm_image(raw, &bytes) && bytes == 0, "asm-opt failure left image bytes");
        sandstone_asm_symbols(raw, &symbols);
        sandstone_asm_relocs(raw, &relocs);
        require(sandstone_asm_section_count(raw) == 0 && symbols == 0 && relocs == 0,
                "asm-opt failure left partial assembly results");
    };
    reject_asm_opt("function preprocess(source) return source end", "must define asm_optimize(source, name)");
    reject_asm_opt("function asm_optimize() end", "must return a string");
    reject_asm_opt("function asm_optimize() return 7 end", "must return a string");
    reject_asm_opt(R"(function asm_optimize(source) return source .. '\0.byte 2\n' end)", "NUL");
    reject_asm_opt(R"(function asm_optimize() return ('x'):rep(16 * 1024 * 1024 + 1) end)", "output exceeds 16 MiB");
    reject_asm_opt(R"(function asm_optimize() return ('x'):rep(128 * 1024 * 1024) end)", "128 MiB limit");
    reject_asm_opt("function asm_optimize() error('rejected by hook') end", "rejected by hook");
    reject_asm_opt("while true do end", "instruction limit exceeded");
    reject_asm_opt("function asm_optimize(source) pcall(function() while true do end end) return source end",
                   "instruction limit exceeded");
    reject_asm_opt(R"(function asm_optimize() return 'partial: .byte 2\n.byte 256\n' end)", "range");
    reject_asm_opt(R"(function asm_optimize() return '.include "not-expanded.s"\n' end)", "unknown directive");

    set_asm_opt("function asm_optimize() return '' end");
    assemble(".byte 5\n", {});
    sandstone_asm_clear_asm_opt_hook(raw);
    assemble(macro_source, {6,7});
    check_asm_opt_evidence("emit32le(0x13);", true);
    check_asm_opt_evidence("emit32le(0x93);", false); // Same width, different bytes.
    check_asm_opt_evidence("emit16le(0x13);", false); // Different width.
    check_asm_opt_evidence(R"(emit32le(0x13); emit_relocation("external", TEST_RELOC);)", false, true);
    check_asm_opt_evidence(nullptr, false); // Missing form.

    std::cout << "assembler SDK, directives, Lua, and asm-opt regressions passed\n";
    return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
