#include "wasmutils/WASM-Binutils.hpp"
#include "../../tests/test.hpp"

#include <set>

using limestone::wasmutils::Error;
namespace core = limestone::wasmutils::core2;

namespace {

using Bytes = std::vector<std::uint8_t>;

template<class Fn>
void rejects(Fn test) {
    try {
        test();
    } catch (const Error&) {
        return;
    }
    CHECK(false && "expected a Core 2.0 binary diagnostic");
}

core::Immediate sample(core::Format format) {
    using F = core::Format;
    switch (format) {
    case F::none:
    case F::zero:
    case F::zero_zero: return {};
    case F::block: return core::BlockType{core::Type::i64};
    case F::index:
    case F::index_zero: return std::uint32_t{300};
    case F::index_pair: return core::IndexPair{0x12345, 7};
    case F::branch_table: return core::BranchTable{{3, 2, 1024}, 8};
    case F::types: return std::vector{core::Type::v128};
    case F::ref_type: return core::Type::externref;
    case F::memory: return core::MemoryArg{0, 0x12345678};
    case F::memory_lane: return core::MemoryLane{{0, 0xffffffff}, 1};
    case F::lane: return std::uint8_t{1};
    case F::i32:
    case F::f32: return std::uint32_t{0x80000000};
    case F::i64:
    case F::f64: return std::uint64_t{0x8123456789abcdef};
    case F::vector_bytes: return core::VectorBytes{4, 7, 0, 0, 1, 2, 3, 30};
    }
    throw std::logic_error("missing immediate sample");
}

void test_inventory() {
    const auto inventory = core::opcodes();
    CHECK(inventory.size() == 437);
    std::set<core::Opcode> unique;
    unsigned simd{};
    for (const auto& entry : inventory) {
        CHECK(unique.insert(entry.opcode).second);
        CHECK(core::lookup(entry.opcode) == &entry);
        CHECK(core::lookup(entry.mnemonic, entry.format == core::Format::types) == &entry);
        const core::Program original{{entry.opcode, sample(entry.format), 0}};
        const auto bytes = core::encode(original);
        CHECK(bytes[0] == entry.opcode.byte);
        CHECK(core::decode(bytes) == original);
        if (entry.opcode.byte == 0xfd)
            ++simd;
    }
    CHECK(simd == 236);
    CHECK(core::lookup("select")->opcode.byte == 0x1b);
    CHECK(core::lookup("select", true)->opcode.byte == 0x1c);
    CHECK(core::lookup("v128.load64_lane")->opcode == core::Opcode{0xfd, 0x57});
    CHECK(core::lookup("i16x8.q15mulr_sat_s")->opcode == core::Opcode{0xfd, 0x82});
    CHECK(core::lookup("f64x2.convert_low_i32x4_u")->opcode == core::Opcode{0xfd, 0xff});
    CHECK(core::lookup("ref.null")->opcode.byte == 0xd0);
    CHECK(core::lookup("memory.copy")->opcode == core::Opcode{0xfc, 10});
    CHECK(!core::lookup("i8x16.relaxed_swizzle"));
    CHECK(!core::lookup(core::Opcode{0xfe, 0x10}));
    CHECK(!core::lookup(core::Opcode{0xfb, 0}));
    CHECK(!core::lookup(core::Opcode{0x01, 1}));
    for (unsigned b = 0; b < 256; ++b) {
        if (b != 0xfc && b != 0xfd && !core::lookup(core::Opcode{static_cast<std::uint8_t>(b), 0}))
            rejects([&] { core::decode(Bytes{static_cast<std::uint8_t>(b)}); });
        if (!core::lookup(core::Opcode{0xfd, b})) {
            Bytes bytes{0xfd, static_cast<std::uint8_t>(b)};
            if (b >= 128)
                bytes.push_back(1);
            rejects([&] { core::decode(bytes); });
        }
    }
}

void test_binary_immediates() {
    const Bytes fixture{
        0x02,0xff,0xff,0xff,0xff,0x0f, // block type index UINT32_MAX (s33)
        0x0e,0x02,0x00,0x80,0x01,0x01, // br_table 0 128 default=1
        0x41,0xff,0x7f,                 // i32.const -1, nonminimal
        0x42,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x7f, // i64.const INT64_MIN
        0x43,0x01,0x00,0xc0,0xff,       // f32 NaN payload
        0x1c,0x01,0x6f,                 // select (result externref)
        0x11,0x80,0x00,0x07,            // call_indirect type=0 (nonminimal), table=7
        0xfc,0x88,0x00,0x80,0x01,0x00, // memory.init data=128, reserved 0
        0xfd,0x55,0x01,0x80,0x02,0x07, // v128.load16_lane align=1 offset=256 lane=7
        0xfd,0xff,0x01,
        0x0b,0x0b
    };
    const auto program = core::decode(fixture);
    CHECK(program.size() == 12);
    CHECK(std::get<core::BlockType>(program[0].immediate).value == core::BlockType{UINT32_MAX}.value);
    CHECK(std::get<core::BranchTable>(program[1].immediate) == core::BranchTable{{0,128},1});
    CHECK(std::get<std::uint32_t>(program[2].immediate) == 0xffffffffU);
    CHECK(std::get<std::uint64_t>(program[3].immediate) == 0x8000000000000000ULL);
    CHECK(std::get<std::uint32_t>(program[4].immediate) == 0xffc00001U);
    CHECK(std::get<std::vector<core::Type>>(program[5].immediate) == std::vector{core::Type::externref});
    CHECK(std::get<core::IndexPair>(program[6].immediate) == core::IndexPair{0,7});
    CHECK(std::get<std::uint32_t>(program[7].immediate) == 128);
    CHECK(std::get<core::MemoryLane>(program[8].immediate) == core::MemoryLane{{1,256},7});
    CHECK(program[9].opcode.extension == 255);
    CHECK(program[1].offset == 6 && program[2].offset == 12 && program[3].offset == 15);
    auto canonical = core::encode(program);
    CHECK(canonical.size() + 3 == fixture.size());
    auto roundtrip = core::decode(canonical);
    for (std::size_t i = 0; i < program.size(); ++i) {
        CHECK(roundtrip[i].opcode == program[i].opcode);
        CHECK(roundtrip[i].immediate == program[i].immediate);
    }
    core::check_expression(program);

    CHECK(core::encode(core::decode(Bytes{0x02,0x80,0x00,0x0b,0x0b})) ==
          Bytes{0x02,0x00,0x0b,0x0b});
    // The compressed value-type alternative must use exactly one byte.
    rejects([] { core::decode(Bytes{0x02,0xff,0x7f}); });
    rejects([] { core::decode(Bytes{0x10,0x80,0x80,0x80,0x80,0x10}); });
    rejects([] { core::decode(Bytes{0x41,0x80,0x80,0x80,0x80,0x08}); });
    rejects([] { core::decode(Bytes{0x41,0x80,0x80,0x80,0x80,0x77}); });
    rejects([] { core::decode(Bytes{0x42,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x01}); });
    rejects([] { core::decode(Bytes{0x0e,0xff,0xff,0xff,0xff,0x0f,0}); });
    rejects([] { core::decode(Bytes{0x1c,0xff,0xff,0xff,0xff,0x0f}); });
    rejects([] { core::decode(Bytes{0x1c,1,0x40}); });
    rejects([] { core::decode(Bytes{0xd0,0x7f}); });
    rejects([] { core::decode(Bytes{0x40,0x80,0x00}); });
    rejects([] { core::decode(Bytes{0xfc,0x0a,0x00,0x01}); });
    rejects([] { core::decode(Bytes{0xfd,0x0c,0x01}); });
    rejects([] { core::decode(Bytes{0xfd,0x55,0x01,0x00}); });
    rejects([] { core::encode(core::Program{{{0x02}, core::Type::i32}}); });
    rejects([] { core::encode(core::Program{{{0xd0}, core::Type::i32}}); });
}

void test_structure() {
    for (const auto& bytes : {Bytes{0x0b}, Bytes{0x04,0x40,0x05,0x02,0x7f,0x0b,0x0b,0x0b}})
        core::check_expression(core::decode(bytes));
    for (const auto& bytes : {Bytes{}, Bytes{0x0b,0x01}, Bytes{0x0b,0x0b}, Bytes{0x05,0x0b},
                              Bytes{0x02,0x40,0x05,0x0b,0x0b}, Bytes{0x02,0x40,0x0b},
                              Bytes{0x04,0x40,0x05,0x05,0x0b,0x0b}})
        rejects([&] { core::check_expression(core::decode(bytes)); });
}

void test_module() {
    // (module (func (local v128) v128.const i32x4 0 0 0 0 drop))
    const Bytes header{0,0x61,0x73,0x6d,1,0,0,0};
    Bytes module = header;
    const Bytes contents{1,4,1,0x60,0,0, 3,2,1,0, 10,25,1,23,
        1,1,0x7b, 0xfd,0x0c,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0x1a,0x0b};
    module.insert(module.end(), contents.begin(), contents.end());
    const auto bodies = core::decode_code_section(module);
    CHECK(bodies.size() == 1 && bodies[0].offset == 22);
    CHECK(bodies[0].locals == std::vector{core::LocalRun{1, core::Type::v128}});
    const auto& instructions = bodies[0].instructions;
    CHECK(instructions.size() == 3 && instructions[0].offset == 25);
    CHECK(instructions[0].opcode == core::Opcode{0xfd, 0x0c});
    CHECK(instructions[2].opcode == core::Opcode{0x0b});
    CHECK(core::decode_code_section(header).empty());

    rejects([&] { core::decode_code_section(std::span(module).first(module.size() - 1)); });
    for (const auto& tail : {Bytes{10,1,1}, Bytes{10,2,0,1}, Bytes{10,1,0,10,1,0},
                             Bytes{10,5,1,3,1,0xff,0}, Bytes{13,0},
                             Bytes{10,4,1,2,0,0x01}}) {
        auto invalid = header;
        invalid.insert(invalid.end(), tail.begin(), tail.end());
        rejects([&] { core::decode_code_section(invalid); });
    }
    rejects([] { core::decode_code_section(Bytes{0,0x61,0x73,0x6d,2,0,0,0}); });
}

} // namespace

int main() {
    return test_main([] {
        test_inventory();
        test_binary_immediates();
        test_structure();
        test_module();
    });
}
