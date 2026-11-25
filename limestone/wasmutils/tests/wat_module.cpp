#include "wasmutils/WASM-Validate.hpp"
#include "wasmutils/WAT-Module.hpp"
#include "../../tests/test.hpp"

using limestone::wasmutils::Error;
namespace core = limestone::wasmutils::core;

namespace {

using Bytes = std::vector<std::uint8_t>;

void test_encoding() {
    const Bytes empty_header{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
    CHECK(core::encode_module(core::assemble_module("(module)")) == empty_header);
    CHECK(core::encode_module(core::assemble_module("")) == empty_header);
    const Bytes answer{
        0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
        1, 5, 1, 0x60, 0, 1, 0x7f, // Type: () -> i32.
        3, 2, 1, 0, // Function of type 0.
        7, 10, 1, 6, 'a', 'n', 's', 'w', 'e', 'r', 0, 0, // Export function 0.
        10, 6, 1, 4, 0, 0x41, 0x2a, 0x0b // No locals, i32.const 42, end.
    };
    CHECK(core::encode_module(core::assemble_module(
        R"((func $f (result i32) i32.const 42) (export "answer" (func $f)))")) == answer);
}

void rejects(std::string_view source, std::string_view at, std::string_view message) {
    try {
        core::assemble_module(source);
    } catch (const Error& error) {
        CHECK(error.offset == source.find(at));
        CHECK(std::string_view(error.what()).find(message) != std::string_view::npos);
        return;
    }
    CHECK(false && "expected a WAT diagnostic");
}

void test_module_fields() {
    constexpr std::string_view source = R"wat(
      (; comments (; can nest ;) ;)
      (module $sample
        (type $unary (func (param i32) (result i32)))
        (import "env" "base" (global $base i32))
        (func $inc (export "imported \u{2603}") (import "env" "inc") (type $unary))
        (export "forward" (func $run))
        (memory $m (export "mem") 1 2)
        (table $t funcref (elem $run))
        (global $g (mut i32) (global.get $base))
        (data $passive "A\ff" "\00\22")
        (data (memory $m) (offset i32.const 8) "B")
        (elem $passive-elem func $run)
        (elem $decl declare funcref (item (ref.func $run)))
        (elem (table $t) (i32.const 0) funcref (ref.func $run))
        (start $init)
        (func $init
          (memory.init $passive (i32.const 0) (i32.const 0) (i32.const 4))
          data.drop $passive
          (table.init $t $passive-elem (i32.const 0) (i32.const 0) (i32.const 1))
          elem.drop $passive-elem)
        ;; A symbolic parameter on an explicit type use can bind the local.
        (func $run (type $unary) (param $x i32) (result i32)
          (local $tmp i32) (local i32 i64)
          local.get $x
          local.set $tmp
          (call $inc (local.get $tmp))))
    )wat";
    auto module = core::assemble_module(source);
    core::validate_module(module);
    CHECK(module.types.size() == 2);
    CHECK(module.types[0] == core::FunctionType{{core::Type::i32}, {core::Type::i32}});
    CHECK(module.imports.size() == 2 && module.imports[0].module == "env");
    CHECK(std::get<std::uint32_t>(module.imports[1].desc) == 0);
    CHECK(module.exports[0].name == "imported \xe2\x98\x83" && module.exports[0].index == 0);
    CHECK(module.exports[1].name == "forward" && module.exports[1].index == 2);
    CHECK(module.start == 1 && module.data_count == 2);
    CHECK(module.tables[0].limits == core::Limits{1, 1});
    CHECK(module.elements.size() == 4 && module.elements[0].mode == core::SegmentMode::active);
    CHECK(module.elements[1].mode == core::SegmentMode::passive);
    CHECK(module.elements[2].mode == core::SegmentMode::declarative);
    CHECK(module.data[0].bytes == Bytes{65, 255, 0, 34});
    CHECK(module.globals[0].type == core::GlobalType{core::Type::i32, true});
    CHECK(module.functions[1].body.locals == std::vector{
        core::LocalRun{2, core::Type::i32}, core::LocalRun{1, core::Type::i64}});
    const auto& body = module.functions[1].body.instructions;
    CHECK(body[0].offset == source.find("local.get $x"));
    CHECK(std::get<std::uint32_t>(body[1].immediate) == 1);
    auto bytes = core::encode_module(module);
    CHECK(core::encode_module(core::decode_module(bytes)) == bytes);
}

void test_expansion_and_control() {
    auto module = core::assemble_module(R"wat(
      (module
        (memory (data "hello" "\0a"))
        (data "also passive")
        (type $base (func (param i32)))
        (func $branches (type $base)
          (local $a i32)
          (block $outer
            loop $inner
              local.get 0
              br_table $inner $outer 2
            end $inner))
        (func (param $p i32) (result i64 i32)
          (if $named (result i64 i32) (local.get $p)
            (then (i64.const -9223372036854775808) (i32.const 0xdead_beef))
            (else i64.const 0xffff_ffff_ffff_ffff i32.const +4)))
        (func (param $p i32) (result i32)
          local.get $p
          if $named (result i32)
            i32.const 1
          else $named
            i32.const 2
          end $named))
    )wat");
    core::validate_module(module);
    CHECK(module.memories[0].limits == core::Limits{1, 1});
    CHECK(module.data[0].bytes == Bytes{'h', 'e', 'l', 'l', 'o', '\n'});
    CHECK(module.data[0].mode == core::SegmentMode::active && !module.data_count);
    const auto& branches = module.functions[0].body.instructions;
    CHECK(std::get<core::BranchTable>(branches[3].immediate) == core::BranchTable{{0, 1}, 2});
    const auto& folded = module.functions[1].body.instructions;
    CHECK(folded[0].opcode == core::Opcode{0x20});
    const auto type = std::get<std::uint32_t>(std::get<core::BlockType>(folded[1].immediate).value);
    CHECK(module.types[type].results == std::vector{core::Type::i64, core::Type::i32});
    CHECK(std::get<std::uint64_t>(folded[2].immediate) == 0x8000000000000000ULL);
    CHECK(std::get<std::uint32_t>(folded[3].immediate) == 0xdeadbeefU);
    CHECK(std::get<std::uint64_t>(folded[5].immediate) == UINT64_MAX);
}

void test_immediates() {
    auto module = core::assemble_module(R"wat(
      (module
        (type $unit (func))
        (table $zero 1 funcref)
        (table $one 1 funcref)
        (memory 1)
        (func
          i32.const 0 call_indirect $one (type $unit)
          (table.copy $one $zero (i32.const 0) (i32.const 0) (i32.const 0))
          (drop (select (result externref) (ref.null extern) (ref.null extern) (i32.const 0)))
          i32.const 0 i32.load offset=0xff_ff align=2 drop
          i32.const 0 i64.load32_u drop
          f32.const -nan:0x40_0001 drop
          f32.const 0x1p-149 drop
          f32.const -0x1p-150 drop
          f64.const +0x1.2p3 drop
          f64.const -1e-10000 drop
          f64.const inf drop
          v128.const i16x8 0 1 -1 -32_768 65535 5 6 7
          v128.const f32x4 0 1.5 -inf nan:0x1
          i8x16.shuffle 0 1 2 3 4 5 6 7 8 9 10 11 12 13 30 31
          i16x8.extract_lane_u 7 drop
          i32.const 0 v128.const i64x2 0 0
          v128.load32_lane offset=2 3 drop
          memory.size drop))
    )wat");
    core::validate_module(module);
    const auto& body = module.functions[0].body.instructions;
    const auto get = [&](std::string_view mnemonic, unsigned occurrence = 0) -> const core::Immediate& {
        for (const auto& ins : body) {
            if (core::lookup(ins.opcode)->mnemonic == mnemonic && occurrence-- == 0)
                return ins.immediate;
        }
        throw std::logic_error("expected instruction");
    };
    CHECK(std::get<core::IndexPair>(get("call_indirect")) == core::IndexPair{0, 1});
    CHECK(std::get<core::IndexPair>(get("table.copy")) == core::IndexPair{1, 0});
    CHECK(std::get<std::vector<core::Type>>(get("select")) == std::vector{core::Type::externref});
    CHECK(std::get<core::MemoryArg>(get("i32.load")) == core::MemoryArg{1, 65535});
    CHECK(std::get<core::MemoryArg>(get("i64.load32_u")) == core::MemoryArg{2, 0});
    CHECK(std::get<std::uint32_t>(get("f32.const", 0)) == 0xffc00001U);
    CHECK(std::get<std::uint32_t>(get("f32.const", 1)) == 1);
    CHECK(std::get<std::uint32_t>(get("f32.const", 2)) == 0x80000000U);
    CHECK(std::get<std::uint64_t>(get("f64.const", 0)) == 0x4022000000000000ULL);
    CHECK(std::get<std::uint64_t>(get("f64.const", 1)) == 0x8000000000000000ULL);
    CHECK(std::get<std::uint64_t>(get("f64.const", 2)) == 0x7ff0000000000000ULL);
    CHECK(std::get<core::VectorBytes>(get("v128.const")) ==
          core::VectorBytes{0, 0, 1, 0, 255, 255, 0, 128, 255, 255, 5, 0, 6, 0, 7, 0});
    CHECK(std::get<core::VectorBytes>(get("v128.const", 1)) ==
          core::VectorBytes{0, 0, 0, 0, 0, 0, 192, 63, 0, 0, 128, 255, 1, 0, 128, 127});
    CHECK(std::get<core::MemoryLane>(get("v128.load32_lane")) == core::MemoryLane{{2, 2}, 3});
}

void test_diagnostics() {
    rejects("(module) junk", "junk", "after module");
    rejects("(module (; unterminated", "(;", "unterminated block comment");
    rejects(R"((module (memory (data "\u{d800}"))))", "\\u{", "scalar value");
    rejects(R"((module (memory (data "\z"))))", "\\z", "string escape");
    rejects(R"((module (func (export "\ff"))))", R"("\ff")", "UTF-8");
    rejects(R"((module (data "a""b")))", R"("a")", "separator");
    rejects("(module (func call $bad))", "$bad", "unknown function");
    rejects("(module (func local.get 2))", "2", "local index out of range");
    rejects("(module (func br 1))", "1", "label index out of range");
    rejects("(module (func block $a end $b))", "$b", "mismatching control label");
    rejects("(module (func (if (i32.const 0) (else))))", "else", "unexpected control delimiter");
    rejects("(module (func block))", "))", "expected end");
    rejects("(module (func (result i32) i32.const 0x1_))", "0x1_", "invalid integer");
    rejects("(module (func i32.const +0xffffffff))", "+0x", "out of range");
    rejects("(module (func i64.const -9223372036854775809))", "-", "out of range");
    rejects("(module (func f32.const nan:0x0))", "nan:", "must be nonzero");
    rejects("(module (func f64.const 1e5000))", "1e", "overflows");
    rejects("(module (memory 1) (func i32.load align=3))", "align=", "power of two");
    rejects("(module (func f32x4.extract_lane 4))", "4))", "lane index out of range");
    rejects("(module (func $a) (func (param i32) (local $b i32) (local $b i32)))",
            "$b i32)))", "duplicate local");
    rejects("(module (func) (import \"m\" \"n\" (func)))", "(import", "import after");
    rejects("(module (type $t (func)) (func (type $t) (param i32)))",
            "$t) (param", "does not match");
    rejects("(module (func return_call 0))", "return_call", "unknown or unsupported");
}

} // namespace

int main() {
    return test_main([] {
        test_encoding();
        test_module_fields();
        test_expansion_and_control();
        test_immediates();
        test_diagnostics();
    });
}
