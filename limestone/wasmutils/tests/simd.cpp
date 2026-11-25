#include "wasmutils/WASM-SIMD.hpp"
#include "../../tests/test.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <utility>

namespace core = limestone::wasmutils::core;

namespace {

using core::Value;
using core::Type;
using Word = std::uint64_t;

Value lanes(unsigned width, std::initializer_list<Word> bits) {
  CHECK(bits.size() <= 128 / width);
  Value vector{Type::v128};
  unsigned bit = 0;
  const auto mask = UINT64_MAX >> (64 - width);
  for (const auto value : bits) {
    (bit < 64 ? vector.bits : vector.high) |= (value & mask) << (bit % 64);
    bit += width;
  }
  return vector;
}

Value i32(std::uint32_t bits) { return {Type::i32, bits}; }
Value i64(Word bits) { return {Type::i64, bits}; }
Word f32(float value) { return std::bit_cast<std::uint32_t>(value); }
Word f64(double value) { return std::bit_cast<Word>(value); }

core::Opcode opcode(std::string_view mnemonic) {
  const auto* info = core::lookup(mnemonic);
  CHECK(info);
  return info->opcode;
}

std::vector<Value> run(std::string_view mnemonic, std::vector<Value> stack = {},
                       core::Immediate immediate = {}, std::span<std::uint8_t> memory = {}) {
  CHECK(core::detail::execute_simd({opcode(mnemonic), std::move(immediate), 91}, stack, memory));
  return stack;
}

Value result(std::string_view mnemonic, std::vector<Value> inputs = {},
             core::Immediate immediate = {}, std::span<std::uint8_t> memory = {}) {
  const auto outputs = run(mnemonic, std::move(inputs), std::move(immediate), memory);
  CHECK(outputs.size() == 1);
  return outputs.front();
}

template<class Run>
void traps(Run run) {
  try {
    run();
  } catch (const core::Trap& error) {
    CHECK(error.offset == 91);
    return;
  }
  CHECK(false && "expected an out-of-bounds SIMD access to trap");
}

void test_inventory() {
  unsigned count = 0;
  for (const auto& info : core::opcodes()) {
    if (info.opcode.byte != 0xfd)
      continue;
    ++count;
    const auto dot = info.mnemonic.find('.');
    const auto name = info.mnemonic.substr(dot + 1);
    const auto prefix = info.mnemonic.substr(0, dot);
    const auto scalar = prefix == "i64x2" ? Type::i64 : prefix == "f32x4" ? Type::f32
        : prefix == "f64x2" ? Type::f64 : Type::i32;
    const Value marker{Type::i32, 17};
    const Value vector{Type::v128};
    std::vector<Value> stack{marker};
    Type output = Type::v128;
    bool has_output = true;
    core::Immediate immediate;
    std::array<std::uint8_t, 64> memory{};
    switch (info.format) {
    case core::Format::memory:
    case core::Format::memory_lane:
      stack.push_back(i32(0));
      immediate = info.format == core::Format::memory
          ? core::Immediate{core::MemoryArg{}} : core::Immediate{core::MemoryLane{}};
      if (name.starts_with("store") || info.format == core::Format::memory_lane)
        stack.push_back(vector);
      has_output = !name.starts_with("store");
      break;
    case core::Format::vector_bytes:
      immediate = core::VectorBytes{};
      if (name == "shuffle")
        stack.insert(stack.end(), 2, vector);
      break;
    case core::Format::lane:
      immediate = std::uint8_t{1};
      stack.push_back(vector);
      if (name == "replace_lane")
        stack.push_back({scalar});
      else
        output = scalar;
      break;
    case core::Format::none:
      if (name == "splat") {
        stack.push_back({scalar});
      } else {
        stack.push_back(vector);
        if (name == "shl" || name.starts_with("shr_")) {
          stack.push_back(i32(0));
        } else if (name == "any_true" || name == "all_true" || name == "bitmask") {
          output = Type::i32;
        } else if (name == "bitselect") {
          stack.insert(stack.end(), 2, vector);
        } else if (name != "not" && name != "abs" && name != "neg" && name != "popcnt" &&
                   name != "ceil" && name != "floor" && name != "trunc" &&
                   name != "nearest" && name != "sqrt" &&
                   !name.starts_with("extend_") && !name.starts_with("extadd_pairwise_") &&
                   !name.starts_with("convert_") && !name.starts_with("trunc_sat_") &&
                   !name.starts_with("promote_") && !name.starts_with("demote_")) {
          stack.push_back(vector);
        }
      }
      break;
    default:
      CHECK(false && "unrecognized SIMD immediate format");
    }
    CHECK(core::detail::execute_simd({info.opcode, immediate, 0}, stack, memory));
    CHECK(stack.size() == (has_output ? 2u : 1u) && stack[0] == marker);
    if (has_output)
      CHECK(stack.back().type == output);
  }
  CHECK(count == 236);
  std::vector<Value> untouched{i32(42)};
  for (unsigned ext = 0; ext <= 256; ++ext) {
    const core::Opcode candidate{0xfd, ext};
    if (!core::lookup(candidate))
      CHECK(!core::detail::execute_simd({candidate, {}}, untouched, {}));
  }
  CHECK(!core::detail::execute_simd({{0x01}, {}}, untouched, {}));
  CHECK(untouched == std::vector<Value>{i32(42)});
}

void test_packing() {
  const core::VectorBytes bytes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const Value a{Type::v128, 0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL};
  const auto b = lanes(8, {16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31});
  CHECK(result("v128.const", {}, bytes) == a);
  CHECK(result("i8x16.shuffle", {a, b}, core::VectorBytes{31, 0, 16, 15, 3, 17, 2, 19,
                30, 8, 29, 9, 21, 14, 18, 4}) ==
        lanes(8, {31, 0, 16, 15, 3, 17, 2, 19, 30, 8, 29, 9, 21, 14, 18, 4}));
  CHECK(result("i8x16.swizzle", {b, lanes(8, {1, 15, 16, 31, 128, 255, 0, 8, 9})}) ==
        lanes(8, {17, 31, 0, 0, 0, 0, 16, 24, 25, 16, 16, 16, 16, 16, 16, 16}));
  CHECK(result("i8x16.splat", {i32(0x180)}) ==
        Value{Type::v128, 0x8080808080808080ULL, 0x8080808080808080ULL});
  const auto replaced = result("i8x16.replace_lane", {a, i32(0x13456)}, std::uint8_t{15});
  CHECK(replaced == Value{Type::v128, a.bits, 0x560e0d0c0b0a0908ULL});
  CHECK(result("i8x16.extract_lane_s", {lanes(8, {0xff, 0x80})}, std::uint8_t{1}) == i32(0xffffff80));
  CHECK(result("i8x16.extract_lane_u", {lanes(8, {0xff, 0x80})}, std::uint8_t{1}) == i32(0x80));
  CHECK(result("i16x8.extract_lane_s", {lanes(16, {0xff00})}, std::uint8_t{0}) == i32(0xffffff00));
  CHECK(result("i64x2.extract_lane", {a}, std::uint8_t{1}) == i64(a.high));
  CHECK(result("f32x4.extract_lane", {lanes(32, {1, 2, 3, 0x7f800123})}, std::uint8_t{3}) ==
        Value{Type::f32, 0x7f800123});
  CHECK(result("f64x2.replace_lane", {a, {Type::f64, 0x8000000000000000ULL}}, std::uint8_t{1}) ==
        Value{Type::v128, a.bits, 0x8000000000000000ULL});
  CHECK(result("v128.bitselect", {a, b, lanes(64, {UINT64_MAX, 0})}) ==
        Value{Type::v128, a.bits, b.high});
  CHECK(result("v128.any_true", {lanes(64, {0, 1})}) == i32(1));
  CHECK(result("v128.any_true", {lanes(64, {})}) == i32(0));
}

void test_integer() {
  const auto a = lanes(8, {0x80, 127, 129, 128, 200, 0xff, 254, 0, 1});
  const auto b = lanes(8, {0x80,   1, 0xff, 2, 128,    1,   5, 1, 0xff});
  CHECK(result("i8x16.add", {a, b}) == lanes(8, {0, 128, 128, 130, 72, 0, 3, 1, 0}));
  CHECK(result("i8x16.add_sat_s", {a, b}) == lanes(8, {128, 127, 128, 130, 128, 0, 3, 1, 0}));
  CHECK(result("i8x16.add_sat_u", {a, b}) == lanes(8, {255, 128, 255, 130, 255, 255, 255, 1, 255}));
  CHECK(result("i8x16.sub_sat_u", {a, b}) == lanes(8, {0, 126, 0, 126, 72, 254, 249, 0, 0}));
  CHECK(result("i8x16.avgr_u", {a, b}) == lanes(8, {128, 64, 192, 65, 164, 128, 130, 1, 128}));
  CHECK(result("i8x16.lt_s", {a, b}) == lanes(8, {0, 0, 255, 255, 0, 255, 255, 255, 0}));
  CHECK(result("i8x16.lt_u", {a, b}) == lanes(8, {0, 0, 255, 0, 0, 0, 0, 255, 255}));
  CHECK(result("i8x16.popcnt", {a}) == lanes(8, {1, 7, 2, 1, 3, 8, 7, 0, 1}));
  CHECK(result("i16x8.bitmask", {lanes(16, {0x8000, 1, 0x8001, 0, 0x7fff, 2, 0x8000, 0xffff})}) ==
        i32(0xc5));
  CHECK(result("i64x2.all_true", {lanes(64, {1, UINT64_C(1) << 63})}) == i32(1));
  CHECK(result("i32x4.all_true", {lanes(32, {1, 1, 1, 0})}) == i32(0));
  CHECK(result("i16x8.shr_s", {lanes(16, {0x8000, 0x7fff}), i32(47)}) == lanes(16, {0xffff, 0}));
  CHECK(result("i16x8.shr_u", {lanes(16, {0x8000, 0x7fff}), i32(47)}) == lanes(16, {1, 0}));
  CHECK(result("i64x2.shl", {lanes(64, {UINT64_MAX, 2}), i32(65)}) == lanes(64, {UINT64_MAX - 1, 4}));
  CHECK(result("i64x2.abs", {lanes(64, {UINT64_C(1) << 63, UINT64_MAX})}) ==
        lanes(64, {UINT64_C(1) << 63, 1}));
  CHECK(result("i64x2.mul", {lanes(64, {UINT64_MAX, UINT64_MAX}), lanes(64, {2, UINT64_MAX})}) ==
        lanes(64, {UINT64_MAX - 1, 1}));
  CHECK(result("i64x2.lt_s", {lanes(64, {UINT64_MAX, 0}), lanes(64, {0, UINT64_MAX})}) ==
        lanes(64, {UINT64_MAX, 0}));
}

void test_width_changes() {
  const auto a = lanes(16, {0x8000, 0xffff, 255, 256, 0x7fff, 0x8080, 0x007f, 0x0080});
  const auto b = lanes(16, {1, 255, 256, 257, 258, 0x7fff, 0, 0xffff});
  CHECK(result("i8x16.narrow_i16x8_u", {a, b}) ==
        lanes(8, {0, 0, 255, 255, 255, 0, 127, 128, 1, 255, 255, 255, 255, 255, 0, 0}));
  CHECK(result("i8x16.narrow_i16x8_s", {a, b}) ==
        lanes(8, {128, 255, 127, 127, 127, 128, 127, 127, 1, 127, 127, 127, 127, 127, 0, 255}));
  CHECK(result("i32x4.extadd_pairwise_i16x8_s", {a}) ==
        lanes(32, {0xffff7fff, 511, 127, 255}));
  CHECK(result("i32x4.extadd_pairwise_i16x8_u", {a}) ==
        lanes(32, {0x17fff, 511, 0x1007f, 255}));
  CHECK(result("i32x4.extend_high_i16x8_s", {a}) ==
        lanes(32, {0x7fff, 0xffff8080, 127, 128}));
  CHECK(result("i32x4.extend_low_i16x8_u", {a}) == lanes(32, {0x8000, 0xffff, 255, 256}));
  CHECK(result("i64x2.extmul_high_i32x4_s", {lanes(32, {9, 9, 0x80000000, 0xffffffff}),
                                            lanes(32, {9, 9, 2, 3})}) ==
        lanes(64, {UINT64_C(0xffffffff00000000), UINT64_MAX - 2}));
  CHECK(result("i64x2.extmul_low_i32x4_u", {lanes(32, {0xffffffff, 0x80000000, 9, 9}),
                                           lanes(32, {0xffffffff, 2, 9, 9})}) ==
        lanes(64, {UINT64_C(0xfffffffe00000001), UINT64_C(0x100000000)}));
  CHECK(result("i32x4.dot_i16x8_s", {lanes(16, {0x8000, 0x8000, 0xffff, 2, 10, 20}),
                                     lanes(16, {0x8000, 0x8000,      2, 3,  1, 0xffff})}) ==
        lanes(32, {0x80000000, 4, 0xfffffff6}));
  CHECK(result("i16x8.q15mulr_sat_s", {lanes(16, {0x8000, 0x8000, 0x4000, 0xffff}),
                                       lanes(16, {0x8000, 0x7fff, 0x4000, 0x3fff})}) ==
        lanes(16, {0x7fff, 0x8001, 0x2000, 0}));
}

void test_float() {
  constexpr Word nan = 0x7f800123;
  const auto a = lanes(32, {f32(0.f), f32(-0.f), nan, f32(2.f)});
  const auto b = lanes(32, {f32(-0.f), f32(0.f), f32(1.f), nan});
  CHECK(result("f32x4.pmin", {a, b}) == a);
  CHECK(result("f32x4.pmax", {a, b}) == a);
  CHECK(result("f32x4.min", {a, b}) == lanes(32, {f32(-0.f), f32(-0.f), 0x7fc00000, 0x7fc00000}));
  CHECK(result("f32x4.max", {a, b}) == lanes(32, {0, 0, 0x7fc00000, 0x7fc00000}));
  CHECK(result("f32x4.ne", {a, b}) == lanes(32, {0, 0, 0xffffffff, 0xffffffff}));
  CHECK(result("f64x2.pmin", {lanes(64, {f64(4.), f64(-4.)}), lanes(64, {f64(3.), f64(-3.)})}) ==
        lanes(64, {f64(3.), f64(-4.)}));
  CHECK(result("f64x2.pmax", {lanes(64, {f64(4.), f64(-4.)}), lanes(64, {f64(3.), f64(-3.)})}) ==
        lanes(64, {f64(4.), f64(-3.)}));
  CHECK(result("f32x4.nearest", {lanes(32, {f32(0.5f), f32(1.5f), f32(2.5f), f32(-0.5f)})}) ==
        lanes(32, {f32(0.f), f32(2.f), f32(2.f), f32(-0.f)}));
  CHECK(result("f64x2.div", {lanes(64, {f64(3.), f64(-4.)}), lanes(64, {f64(2.), f64(-2.)})}) ==
        lanes(64, {f64(1.5), f64(2.)}));
  CHECK(result("i32x4.trunc_sat_f32x4_u",
               {lanes(32, {f32(-1.75f), f32(7.75f), 0x7f800000, nan})}) ==
        lanes(32, {0, 7, 0xffffffff, 0}));
  CHECK(result("i32x4.trunc_sat_f64x2_s_zero",
               {lanes(64, {UINT64_C(0xfff0000000000000), UINT64_C(0x7ff0000000000000)})}) ==
        lanes(32, {0x80000000, 0x7fffffff, 0, 0}));
  CHECK(result("f32x4.demote_f64x2_zero", {lanes(64, {f64(-0.), f64(1.25)})}) ==
        lanes(32, {f32(-0.f), f32(1.25f), 0, 0}));
  CHECK(result("f64x2.promote_low_f32x4", {lanes(32, {f32(4.f), f32(-0.f), 1, nan})}) ==
        lanes(64, {f64(4.), f64(-0.)}));
  CHECK(result("f64x2.convert_low_i32x4_s", {lanes(32, {0x80000000, 0xffffffff, 8, 9})}) ==
        lanes(64, {f64(-2147483648.), f64(-1.)}));
  CHECK(result("f64x2.convert_low_i32x4_u", {lanes(32, {0x80000000, 0xffffffff, 8, 9})}) ==
        lanes(64, {f64(2147483648.), f64(4294967295.)}));
}

void test_memory() {
  std::array<std::uint8_t, 40> memory{};
  const std::array<std::uint8_t, 8> pattern{0x80, 0xff, 0x7f, 0x01, 0x00, 0x80, 0xff, 0x7f};
  std::copy(pattern.begin(), pattern.end(), memory.begin() + 5);
  const core::MemoryArg offset{0, 2};
  CHECK(result("v128.load8x8_s", {i32(3)}, offset, memory) ==
        lanes(16, {0xff80, 0xffff, 0x7f, 1, 0, 0xff80, 0xffff, 0x7f}));
  CHECK(result("v128.load8x8_u", {i32(3)}, offset, memory) ==
        lanes(16, {0x80, 0xff, 0x7f, 1, 0, 0x80, 0xff, 0x7f}));
  CHECK(result("v128.load16x4_s", {i32(3)}, offset, memory) ==
        lanes(32, {0xffffff80, 0x017f, 0xffff8000, 0x7fff}));
  CHECK(result("v128.load16_splat", {i32(3)}, offset, memory) ==
        lanes(64, {0xff80ff80ff80ff80ULL, 0xff80ff80ff80ff80ULL}));
  CHECK(result("v128.load32_zero", {i32(3)}, offset, memory) == lanes(64, {0x017fff80, 0}));
  CHECK(result("v128.load64_zero", {i32(3)}, offset, memory) == lanes(64, {0x7fff8000017fff80ULL, 0}));
  const auto value = lanes(64, {0x0123456789abcdefULL, 0xfedcba9876543210ULL});
  CHECK(run("v128.store", {i32(1), value}, core::MemoryArg{4, 2}, memory).empty());
  CHECK(memory[3] == 0xef && memory[10] == 0x01 && memory[11] == 0x10 && memory[18] == 0xfe);
  CHECK(result("v128.load", {i32(1)}, core::MemoryArg{4, 2}, memory) == value);
  CHECK(result("v128.load16_lane", {i32(3), value}, core::MemoryLane{{1, 0}, 7}, memory) ==
        lanes(64, {value.bits, 0xcdefba9876543210ULL}));
  CHECK(run("v128.store64_lane", {i32(32), value}, core::MemoryLane{{3, 0}, 1}, memory).empty());
  CHECK(result("v128.load64_lane", {i32(32), value}, core::MemoryLane{{3, 0}, 0}, memory) ==
        lanes(64, {value.high, value.high}));
  const auto snapshot = memory;
  traps([&] { run("v128.store", {i32(25), value}, core::MemoryArg{}, memory); });
  traps([&] { run("v128.store32_lane", {i32(37), value}, core::MemoryLane{{}, 3}, memory); });
  traps([&] { run("v128.load8_splat", {i32(UINT32_MAX)}, core::MemoryArg{0, 1}, memory); });
  traps([] { run("v128.load", {i32(0)}, core::MemoryArg{}); });
  CHECK(memory == snapshot); // A trapping store never performs a partial write.
}

void test_instance() {
  // An initialized vector global passes through a local and a host call, then
  // both halves remain observable after storing to and loading from memory.
  core::Module module;
  module.types.push_back({{Type::v128}, {Type::v128}});
  module.types.push_back({{}, {Type::v128}});
  module.imports.push_back({"host", "flip", std::uint32_t{0}});
  module.memories.push_back({{1, 1}});
  module.globals.push_back({{Type::v128, false},
      {{{0xfd, 0x0c}, core::VectorBytes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
       {{0x0b}, {}}}});
  module.functions.push_back({1, {0, {{1, Type::v128}},
      {{{0x41}, std::uint32_t{65520}}, {{0x23}, std::uint32_t{0}}, {{0x21}, std::uint32_t{0}},
       {{0x20}, std::uint32_t{0}}, {{0x10}, std::uint32_t{0}}, {{0xfd, 0x4d}, {}},
       {{0xfd, 0x0b}, core::MemoryArg{4, 0}, 91}, {{0x41}, std::uint32_t{65520}},
       {{0xfd, 0x00}, core::MemoryArg{4, 0}}, {{0x0b}, {}}}}});
  const std::array host{core::HostFunction{"host", "flip", [](std::span<const Value> values) {
    CHECK(values.size() == 1 && values[0].type == Type::v128);
    return std::vector<Value>{{Type::v128, values[0].high, values[0].bits}};
  }}};
  core::Instance instance(module, host);
  CHECK(instance.invoke(1) == std::vector<Value>{{Type::v128, 0xf0f1f2f3f4f5f6f7ULL, 0xf8f9fafbfcfdfeffULL}});
  module.memories.front().limits.min = 0;
  core::Instance zero_page(std::move(module), host);
  traps([&] { zero_page.invoke(1); });
}

} // namespace

int main() {
  test_inventory();
  test_packing();
  test_integer();
  test_width_changes();
  test_float();
  test_memory();
  test_instance();
}
