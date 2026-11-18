#include "wasmutils/WASM-Binutils.hpp"
#include "wasmutils/WASMAssembler.hpp"
#include "wasmutils/WASMDisassemble.hpp"
#include "wasmutils/WASM-CC.hpp"
#include "wasmutils/WASM-Runtime.hpp"
#include "wasmutils/WATCompiler.hpp"
#include "wasmutils/WIT-Visitor.hpp"
#include "wasmutils/WITParser.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace wasm = limestone::wasmutils;
namespace {

void check(bool condition, std::string_view reason) {
    if (!condition)
        throw std::runtime_error(std::string(reason));
}

template<class Fn>
void rejects(Fn fn) {
    try {
        fn();
    } catch (const wasm::Error&) {
        return;
    }
    throw std::runtime_error("invalid input was accepted");
}

void check_profile(const char* path) {
    std::ifstream source(path);
    check(bool(source), "cannot open WASM.aml");
    std::array<bool, 256> listed{};
    std::string mnemonic, line;
    while (std::getline(source, line)) {
        constexpr std::string_view instruction = "  (instruction ";
        constexpr std::string_view encoding = "    (encoding (base \"";
        if (line.starts_with(instruction)) {
            check(mnemonic.empty(), "missing instruction encoding in WASM.aml");
            mnemonic = line.substr(instruction.size());
        } else if (!mnemonic.empty() && line.starts_with(encoding)) {
            const auto hex = line.substr(encoding.size(), line.find('"', encoding.size()) - encoding.size());
            if (mnemonic.ends_with("-sleb7"))
                mnemonic.resize(mnemonic.size() - 6);
            check(hex.size() == 2 || hex.size() == 4, "unexpected instruction encoding");
            const auto opcode = std::stoul(hex.substr(0, 2), nullptr, 16);
            check(!listed[opcode], "duplicate WASM.aml opcode");
            listed[opcode] = true;
            const auto* by_name = wasm::opcode_info(mnemonic);
            check(by_name && by_name->opcode == opcode && by_name->mnemonic == mnemonic,
                  "WASM.aml instruction does not match the IR table");
            check(by_name == wasm::opcode_info(by_name->opcode), "mismatched opcode lookups");
            const unsigned width = mnemonic.starts_with("i32.") ? 32 : mnemonic.starts_with("i64.") ? 64 : 0;
            check(by_name->width == width, "unexpected instruction width");
            if (hex.size() == 4) {
                check(hex.substr(2) == "00" && by_name->operation == wasm::Operation::constant,
                      "unexpected operand encoding");
                for (std::int32_t value = -64; value <= 63; ++value) {
                    const wasm::Program original{{by_name->opcode, value, 0}};
                    auto bytes = wasm::assemble(original);
                    check(bytes.size() == 2 && bytes[0] == opcode &&
                          bytes[1] == (static_cast<std::uint32_t>(value) & 0x7f), "wrong signed LEB7 encoding");
                    auto decoded = wasm::disassemble(bytes);
                    check(decoded.size() == 1 && decoded[0].opcode == opcode &&
                          decoded[0].immediate == value && decoded[0].offset == 0, "wrong signed LEB7 decoding");
                }
            } else {
                auto bytes = wasm::assemble(wasm::Program{{by_name->opcode, 0, 0}});
                check(bytes.size() == 1 && bytes.front() == opcode, "wrong opcode encoding");
                auto decoded = wasm::disassemble(bytes);
                check(decoded.size() == 1 && decoded.front().opcode == opcode, "wrong opcode decoding");
            }
            mnemonic.clear();
        }
    }
    check(mnemonic.empty(), "missing final instruction encoding");
    for (unsigned i = 0; i < listed.size(); ++i)
        check(listed[i] == bool(wasm::opcode_info(static_cast<std::uint8_t>(i))), "opcode outside WASM.aml");
}

struct Case {
    std::string_view wat;
    wasm::ValueType type;
    std::uint64_t bits;
};

constexpr auto i32 = wasm::ValueType::i32;
constexpr auto i64 = wasm::ValueType::i64;
constexpr Case cases[] = {
    {"i32.const -64", i32, 0xffffffc0}, {"i64.const -0x4_0", i64, 0xffffffffffffffc0},
    {"(i32.add (i32.const -1) (i32.const 1))", i32, 0},
    {"i64.const -1 i64.const 1 i64.add", i64, 0},
    {"i32.const 0 i32.const 1 i32.sub", i32, 0xffffffff},
    {"i64.const 0 i64.const 1 i64.sub", i64, 0xffffffffffffffff},
    {"i32.const -1 i32.const 2 i32.mul", i32, 0xfffffffe},
    {"i64.const -1 i64.const -1 i64.mul", i64, 1},
    {"i64.const 1 i64.const 32 i64.shl i64.const 1 i64.add "
     "i64.const 1 i64.const 32 i64.shl i64.const 1 i64.add i64.mul", i64, 0x0000000200000001},
    {"i32.const -1 i32.const 3 i32.and", i32, 3},
    {"i64.const -1 i64.const 3 i64.and", i64, 3},
    {"i32.const 8 i32.const 3 i32.or", i32, 11},
    {"i64.const 1 i64.const 32 i64.shl i64.const 3 i64.or", i64, 0x0000000100000003},
    {"i32.const -1 i32.const 2 i32.xor", i32, 0xfffffffd},
    {"i64.const -1 i64.const 2 i64.xor", i64, 0xfffffffffffffffd},
    {"i32.const 1 i32.const -1 i32.shl", i32, 0x80000000},
    {"i64.const 1 i64.const -1 i64.shl", i64, 0x8000000000000000},
    {"i32.const -2 i32.const 1 i32.shr_s", i32, 0xffffffff},
    {"i64.const 1 i64.const -1 i64.shl i64.const 33 i64.shr_s", i64, 0xffffffffc0000000},
    {"i32.const -1 i32.const 33 i32.shr_u", i32, 0x7fffffff},
    {"i64.const -1 i64.const 32 i64.shr_u", i64, 0x00000000ffffffff},
    {"i64.const -1 i64.const 31 i64.shr_u", i64, 0x00000001ffffffff},
    {"i32.const 1 i32.const 32 i32.rotl", i32, 1},
    {"i32.const -1 i32.const 1 i32.xor i32.const 1 i32.rotl", i32, 0xfffffffd},
    {"i64.const 3 i64.const -1 i64.rotl", i64, 0x8000000000000001},
    {"i64.const 1 i64.const -1 i64.shl i64.const 1 i64.rotl", i64, 1},
    {"i32.const 3 i32.const 1 i32.rotr", i32, 0x80000001},
    {"i64.const 3 i64.const 1 i64.rotr", i64, 0x8000000000000001},
    {"i32.const 0 i32.clz", i32, 32}, {"i64.const 0 i64.clz", i64, 64},
    {"i32.const 1 i32.clz", i32, 31}, {"i64.const 1 i64.const 32 i64.shl i64.clz", i64, 31},
    {"i32.const 0 i32.ctz", i32, 32}, {"i64.const 0 i64.ctz", i64, 64},
    {"i32.const -1 i32.const -1 i32.shl i32.ctz", i32, 31},
    {"i64.const -1 i64.const -1 i64.shl i64.ctz", i64, 63},
    {"i32.const -1 i32.popcnt", i32, 32}, {"i64.const -1 i64.popcnt", i64, 64},
    {"i32.const 0 i32.eqz", i32, 1}, {"i32.const -1 i32.eqz", i32, 0},
    {"i64.const 0 i64.eqz", i32, 1}, {"i64.const 1 i64.const 32 i64.shl i64.eqz", i32, 0},
    {"i32.const 4 i32.const 5 i32.const 0 select", i32, 5},
    {"i64.const 4 i64.const 5 i32.const -1 select", i64, 4},
    {"(i32.const 42) (drop (i64.const 7)) nop", i32, 42},
};

std::string wat_fixture() {
    std::string result = ";; Test all non-trapping forms in the selected ISA.\n";
    for (const auto& sample : cases) {
        result += sample.wat;
        result += '\n';
    }
    return result;
}

void check_program() {
    const auto program = wasm::compile_wat(wat_fixture());
    const auto bytes = wasm::assemble(program);
    const auto binary = wasm::disassemble(bytes);
    check(wasm::assemble(binary) == bytes, "byte code does not round trip");
    check(wasm::emit_c(program) == wasm::emit_c(binary), "source offsets changed generated C");

    const auto validation = wasm::validate(binary);
    check(!validation.always_traps && validation.results.size() == std::size(cases), "wrong result shape");
    const auto execution = wasm::execute(binary);
    check(!execution.trapped && execution.stack.size() == std::size(cases), "runtime result stack has wrong size");
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        const auto& value = execution.stack[i];
        if (validation.results[i] != cases[i].type || value.type != cases[i].type || value.bits != cases[i].bits)
            throw std::runtime_error("wrong validation/execution result for: " + std::string(cases[i].wat));
    }

    const auto module = wasm::compile_wat("(module (func (result i32) "
                                          "(; outer (; nested ;) ;) (i32.add (i32.const 40) (i32.const 2))))");
    check(wasm::execute(module).stack[0].bits == 42, "wrong WAT function result");
    check(wasm::execute(wasm::compile_wat("nop")).stack.empty(), "nop changed the stack");
    const auto trap = wasm::compile_wat("i32.const 7 unreachable i64.add drop");
    check(wasm::validate(trap).always_traps, "validator did not recognize unconditional trap");
    const auto trapped = wasm::execute(trap);
    check(trapped.trapped && trapped.stack.empty(), "wrong trap result");
    const auto trapped_function = wasm::compile_wat("(module (func (result i32) unreachable))");
    check(wasm::execute(trapped_function).trapped, "wrapped function did not trap");
    const auto trapped_suffix = wasm::compile_wat("(module (func (result i32 i64) unreachable i64.const 0))");
    check(wasm::execute(trapped_suffix).trapped, "wrapped function with a valid explicit suffix did not trap");
    constexpr std::string_view mixed_text = ";; CR ends a line comment\r(i32.sub i32.const 6 (i32.const +0x2))";
    const auto mixed = wasm::compile_wat(mixed_text);
    check(mixed.size() == 3 && mixed.back().offset == mixed_text.find("i32.sub"),
          "folded lowering did not preserve the enclosing mnemonic's source location");
    const auto mixed_result = wasm::execute(mixed);
    check(mixed_result.stack.size() == 1 && mixed_result.stack.front().bits == 4,
          "folded instructions were not evaluated in stack order");

    rejects([] { wasm::compile_wat("i64.const 64"); });
    rejects([] { wasm::compile_wat("i64.const -65"); });
    rejects([] { wasm::compile_wat("(i32.const 1"); });
    rejects([] { wasm::compile_wat("(; unfinished"); });
    rejects([] { wasm::compile_wat("i32.const 1__2"); });
    rejects([] { wasm::compile_wat("i32.const 1 i64.eqz"); });
    rejects([] { wasm::compile_wat("i32.const 0 i64.const 1 i32.const 1 select"); });
    rejects([] { wasm::compile_wat("unreachable i64.const 1 i32.eqz"); });
    rejects([] { wasm::compile_wat("(module (func (result i64) i32.const 0))"); });
    rejects([] { wasm::compile_wat("(module (func (result i32) unreachable i64.const 0))"); });
    rejects([] { wasm::compile_wat("(module (func (result i32) unreachable i32.const 0 i32.const 1))"); });
    rejects([] { wasm::compile_wat("(module (func unreachable select))"); });
    rejects([] { wasm::compile_wat("(module (func (param i32) nop))"); });
    rejects([] { wasm::compile_wat("(module (func nop) (func nop))"); });
    rejects([] {
        std::string nested;
        for (int i = 0; i < 129; ++i)
            nested += "(nop ";
        nested.append(129, ')');
        wasm::compile_wat(nested);
    });
    rejects([] {
        std::string nested;
        for (int i = 0; i < 129; ++i)
            nested += "(;";
        for (int i = 0; i < 129; ++i)
            nested += ";)";
        wasm::compile_wat(nested);
    });
    rejects([] { wasm::compile_wat("block nop end"); });
    rejects([] { wasm::disassemble(std::array<std::uint8_t, 1>{0x41}); });
    rejects([] { wasm::disassemble(std::array<std::uint8_t, 3>{0x42, 0x80, 0x00}); });
    rejects([] { wasm::disassemble(std::array<std::uint8_t, 1>{0xfd}); });
    rejects([] { wasm::assemble(wasm::Program{{0x01, -1, 0}}); });
    rejects([] { wasm::assemble(wasm::Program{{0x41, 64, 0}}); });
    rejects([] { wasm::execute(wasm::Program{{0x00}, {0x42, 0}, {0x45}}); });
}

void write_fixture(const std::filesystem::path& path, std::string_view data) {
    std::ofstream out(path, std::ios::binary);
    check(bool(out << data << std::flush), "cannot write fixture");
}

void check_wit() {
    auto document = wasm::parse_wit("/* outer /* nested */ */ "
        "package demo:math@1.0.0; interface sample { checked: func(%type: u32,) -> bool; }");
    struct Functions final : wasm::WitVisitor {
        unsigned count{};
        void visit(const wasm::WitFunction& fn) override {
            check(fn.name.text == "checked" && fn.parameters.size() == 1 &&
                  fn.parameters[0].name.text == "type" && fn.result &&
                  fn.result->scalar == wasm::WitScalar::Bool, "wrong WIT function AST");
            ++count;
        }
    } functions;
    wasm::walk_wit(document, functions);
    check(functions.count == 1, "unexpected WIT visitor count");
    check(wasm::emit_wit_c(document).find("wit_demo_W_math_W_v1v0v0_W_sample_W_checked") != std::string::npos,
          "incorrect WIT symbol mangling");
    try {
        wasm::parse_wit("interface sample {\n  item: func();\n  ITEM: func();\n}");
        throw std::runtime_error("duplicate WIT symbol accepted");
    } catch (const wasm::WitError& error) {
        check(error.location.line == 3 && error.location.column == 3, "wrong WIT duplicate location");
    }
    document.interfaces[0].functions[0].name.text = "bad();";
    try {
        wasm::emit_wit_c(document);
        throw std::runtime_error("invalid WIT AST identifier accepted");
    } catch (const wasm::WitError&) {
        // Public AST callers must obey the same identifier validation as text callers.
    }
}

void write_fixtures(const std::filesystem::path& dir) {
    const auto text = wat_fixture();
    write_fixture(dir / "fixture.wat", text);
    const auto binary = wasm::assemble(wasm::compile_wat(text));
    write_fixture(dir / "fixture.ops", {reinterpret_cast<const char*>(binary.data()), binary.size()});
    std::ostringstream expected;
    for (const auto& sample : cases) {
        expected << "    { " << (sample.type == i32 ? 32 : 64)
                 << ", 0x" << std::hex << static_cast<std::uint32_t>(sample.bits)
                 << "UL, 0x" << (sample.bits >> 32) << "UL },\n" << std::dec;
    }
    write_fixture(dir / "expected.inc", expected.str());
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--fixtures") {
            write_fixtures(argv[2]);
        } else {
            check(argc == 2, "expected the path to WASM.aml");
            check_profile(argv[1]);
            check_program();
            check_wit();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
