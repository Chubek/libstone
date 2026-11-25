#include "wasmutils/WASM-Instance.hpp"
#include "wasmutils/WASM-Module.hpp"

#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wasm = limestone::wasmutils::core;
namespace {

using wasm::Instruction;
using wasm::Program;
using wasm::Type;
using wasm::Value;

void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

Instruction op(std::uint8_t byte, wasm::Immediate immediate = {}, std::size_t offset = 0) {
  return {{byte, 0}, std::move(immediate), offset};
}

Instruction bulk(std::uint32_t extension, wasm::Immediate immediate = {}, std::size_t offset = 0) {
  return {{0xfc, extension}, std::move(immediate), offset};
}

Instruction i32(std::uint32_t value) { return op(0x41, value); }
Instruction i64(std::uint64_t value) { return op(0x42, value); }
Instruction f32(float value) { return op(0x43, std::bit_cast<std::uint32_t>(value)); }
Instruction f64(double value) { return op(0x44, std::bit_cast<std::uint64_t>(value)); }
Instruction index(std::uint8_t byte, std::uint32_t value) { return op(byte, value); }
Instruction memory(std::uint8_t byte, std::uint32_t displacement = 0, std::size_t offset = 0) {
  return op(byte, wasm::MemoryArg{0, displacement}, offset);
}

std::uint32_t add_function(wasm::Module& module, std::vector<Type> results, Program body,
                           std::vector<Type> params = {}) {
  const auto type = static_cast<std::uint32_t>(module.types.size());
  const auto function = static_cast<std::uint32_t>(module.functions.size());
  module.types.push_back({std::move(params), std::move(results)});
  body.push_back(op(0x0b));
  module.functions.push_back({type, {0, {}, std::move(body)}});
  return function;
}

Value eval(Type type, Program program) {
  wasm::Module module;
  add_function(module, {type}, std::move(program));
  const auto result = wasm::Instance(std::move(module)).invoke(std::uint32_t{0});
  check(result.size() == 1, "wrong result count");
  return result.front();
}

template<class Call>
void expect_trap(Call&& call, std::size_t offset, const char* message) {
  try {
    call();
  } catch (const wasm::Trap& fault) {
    check(fault.offset == offset, "wrong trap source offset");
    return;
  }
  throw std::runtime_error(message);
}

void test_integer() {
  using enum Type;
  check(eval(i32, {::i32(0xffffffff), ::i32(2), op(0x6a)}) == Value{i32, 1},
        "i32 addition does not wrap");
  check(eval(i64, {::i64(0x100000000ULL), ::i64(0x100000000ULL), op(0x7e)}) == Value{i64, 0},
        "i64 multiplication does not wrap");
  check(eval(i32, {::i32(0xffffffff), ::i32(1), op(0x48)}) == Value{i32, 1} &&
        eval(i32, {::i32(0xffffffff), ::i32(1), op(0x49)}) == Value{i32, 0},
        "signed and unsigned integer comparisons did not distinguish the sign bit");
  check(eval(i64, {::i64(0), op(0x7a)}) == Value{i64, 64}, "i64.ctz of zero");
  check(eval(i32, {::i32(0x80000000), ::i32(33), op(0x75)}) == Value{i32, 0xc0000000},
        "i32 arithmetic shift does not mask shift counts or propagate the sign");
  check(eval(i64, {::i64(1), ::i64(65), op(0x8a)}) == Value{i64, 0x8000000000000000ULL},
        "i64 rotation does not mask shift counts");
  check(eval(i64, {::i64(0xfffffffffffffff9ULL), ::i64(3), op(0x81)}) ==
            Value{i64, 0xffffffffffffffffULL},
        "signed remainder must preserve the dividend's sign");
  check(eval(i32, {::i32(0x80000000), ::i32(0xffffffff), op(0x6f)}) == Value{i32, 0} &&
        eval(i64, {::i64(0x8000000000000000ULL), ::i64(0xffffffffffffffffULL), op(0x81)}) ==
            Value{i64, 0},
        "signed remainder of MIN by -1 must not trap");
  expect_trap([&] { eval(i32, {::i32(0x80000000), ::i32(0xffffffff), op(0x6d, {}, 19)}); },
              19, "i32 signed division overflow did not trap");
  expect_trap([&] { eval(i64, {::i64(0x8000000000000000ULL), ::i64(0xffffffffffffffffULL),
                              op(0x7f, {}, 20)}); },
              20, "i64 signed division overflow did not trap");
  expect_trap([&] { eval(i64, {::i64(7), ::i64(0), op(0x82, {}, 21)}); },
              21, "integer remainder by zero did not trap");
  check(eval(i64, {::i64(0x1234567880000001ULL), op(0xc4)}) == Value{i64, 0xffffffff80000001ULL} &&
        eval(i32, {::i64(0x1234567880000001ULL), op(0xa7)}) == Value{i32, 0x80000001},
        "sign extension or wrapping conversion is incorrect");
}

void test_float() {
  using enum Type;
  constexpr std::uint32_t negative_zero = 0x80000000;
  constexpr std::uint32_t signaling_nan = 0x7f800001;
  check(eval(f32, {::f32(-0.0f), ::f32(0.0f), op(0x96)}) == Value{f32, negative_zero} &&
        eval(f32, {::f32(-0.0f), ::f32(0.0f), op(0x97)}) == Value{f32, 0},
        "min/max do not choose the specified zero sign");
  check(eval(f32, {::f32(0.0f), op(0x43, signaling_nan), op(0x96)}) == Value{f32, 0x7fc00000},
        "min did not produce an arithmetic NaN");
  check(eval(f32, {op(0x43, signaling_nan), ::f32(-0.0f), op(0x98)}) ==
            Value{f32, 0xff800001},
        "copysign changed the signaling NaN payload");
  check(eval(i32, {op(0x43, signaling_nan), op(0xbc)}) == Value{i32, signaling_nan},
        "reinterpretation changed the signaling NaN payload");
  check(eval(i32, {op(0x43, signaling_nan), ::f32(0), op(0x5b)}) == Value{i32, 0} &&
        eval(i32, {op(0x43, signaling_nan), ::f32(0), op(0x5c)}) == Value{i32, 1},
        "NaN comparison is not unordered");
  check(eval(f32, {::f32(-0.5f), op(0x90)}) == Value{f32, negative_zero} &&
        eval(f64, {::f64(2.5), op(0x9e)}) == Value{f64, std::bit_cast<std::uint64_t>(2.0)} &&
        eval(f64, {::f64(3.5), op(0x9e)}) == Value{f64, std::bit_cast<std::uint64_t>(4.0)},
        "nearest rounding is not ties-to-even");
  check(eval(f32, {::f32(1), ::f32(-0.0f), op(0x95)}) == Value{f32, 0xff800000},
        "floating division by zero must produce signed infinity");
  check(eval(f32, {::f64(0x1.fffffeffffp127), op(0xb6)}) == Value{f32, 0x7f7fffff} &&
        eval(f32, {::f64(0x1.ffffffp127), op(0xb6)}) == Value{f32, 0x7f800000},
        "f64-to-f32 demotion did not round at the overflow midpoint");
  check(eval(f32, {::f64(0x1p-149), op(0xb6)}) == Value{f32, 1},
        "f64-to-f32 demotion lost a subnormal");

  std::fenv_t original_environment;
  check(std::fegetenv(&original_environment) == 0, "could not read host floating-point environment");
  check(std::fesetround(FE_UPWARD) == 0 && std::feclearexcept(FE_ALL_EXCEPT) == 0 &&
        std::feraiseexcept(FE_INEXACT) == 0, "could not set host floating-point environment");
  Value added;
  Value rounded;
  Value converted;
  Value divided;
  try {
    added = eval(f32, {::f32(1.0f), ::f32(0x1p-24f), op(0x92)});
    rounded = eval(f64, {::f64(2.5), op(0x9e)});
    converted = eval(f32, {::i32(0x1000001), op(0xb3)});
    divided = eval(f64, {::f64(1.0), ::f64(0.0), op(0xa3)});
  } catch (...) {
    std::fesetenv(&original_environment);
    throw;
  }
  const auto round_after = std::fegetround();
  const auto flags_after = std::fetestexcept(FE_ALL_EXCEPT);
  std::fesetenv(&original_environment);
  check(added == Value{f32, 0x3f800000} &&
        rounded == Value{f64, std::bit_cast<std::uint64_t>(2.0)} &&
        converted == Value{f32, 0x4b800000} && divided == Value{f64, 0x7ff0000000000000ULL} &&
        round_after == FE_UPWARD && flags_after == FE_INEXACT,
        "Wasm arithmetic did not isolate host floating-point modes and flags");
}

void test_float_to_integer() {
  using enum Type;
  check(eval(i32, {::f64(-0.9), op(0xab)}) == Value{i32, 0},
        "unsigned truncation of a fraction in (-1, 0) must succeed");
  check(eval(i32, {::f64(-2147483648.75), op(0xaa)}) == Value{i32, 0x80000000},
        "signed truncation must range-check after truncating");
  check(eval(i64, {::f64(0x1.fffffffffffffp63), op(0xb1)}) == Value{i64, 0xfffffffffffff800ULL},
        "largest representable f64 below the i64 unsigned bound did not convert");
  expect_trap([&] { eval(i64, {::f64(0x1p64), op(0xb1, {}, 91)}); },
              91, "unsigned truncation of 2^64 did not trap");
  expect_trap([&] { eval(i32, {::f64(0x1p31), op(0xaa, {}, 92)}); },
              92, "signed truncation of 2^31 did not trap");
  expect_trap([&] { eval(i32, {::f64(-2147483649.0), op(0xaa, {}, 93)}); },
              93, "signed truncation below -2^31 did not trap");
  expect_trap([&] { eval(i32, {::f32(std::numeric_limits<float>::quiet_NaN()), op(0xa8, {}, 94)}); },
              94, "truncation of NaN did not trap");
  check(eval(i64, {::f64(0x1p64), bulk(7)}) == Value{i64, 0xffffffffffffffffULL} &&
        eval(i32, {::f64(-std::numeric_limits<double>::infinity()), bulk(2)}) ==
            Value{i32, 0x80000000} &&
        eval(i64, {::f32(std::numeric_limits<float>::quiet_NaN()), bulk(4)}) == Value{i64, 0},
        "saturating truncation did not clamp overflow, infinity, or NaN");
}

void test_memory() {
  wasm::Module module;
  module.memories.push_back({{1, 2}});
  module.data = {
    {wasm::SegmentMode::active, 0, {i32(1), op(0x0b)}, {0x80, 0xff}},
    {wasm::SegmentMode::passive, 0, {}, {5, 6, 7, 8}},
  };
  module.data_count = 2;
  const auto narrow = add_function(module, {Type::i64, Type::i32, Type::f32}, {
    i32(1), memory(0x32),
    i32(1), memory(0x2f),
    i32(3), op(0x43, std::uint32_t{0x7f800001}), memory(0x38),
    i32(3), memory(0x2a),
  });
  const auto copy = add_function(module, {Type::i32}, {
    i32(8), i32(0), i32(4), bulk(8, std::uint32_t{1}), // memory.init
    i32(9), i32(8), i32(3), bulk(10), // overlap towards higher addresses
    i32(8), i32(9), i32(3), bulk(10), // overlap towards lower addresses
    i32(9), i32(0x123), i32(1), bulk(11), // fill uses only the low byte
    i32(8), memory(0x28),
  });
  const auto last_byte = add_function(module, {Type::i32}, {
    i32(65'535), memory(0x2d),
  });
  const auto bad_store = add_function(module, {}, {
    i32(65'535), i32(0x5a), memory(0x3a),
    i32(65'535), i32(0), memory(0x3b, 0, 201),
  });
  const auto bad_fill = add_function(module, {}, {
    i32(65'535), i32(0), i32(2), bulk(11, {}, 202),
  });
  const auto bad_copy = add_function(module, {}, {
    i32(65'535), i32(0), i32(2), bulk(10, {}, 203),
  });
  const auto bad_init = add_function(module, {}, {
    i32(65'535), i32(0), i32(2), bulk(8, std::uint32_t{1}, 204),
  });
  const auto bad_wrap = add_function(module, {Type::i32}, {
    i32(0xffffffff), memory(0x2d, 2, 205),
  });
  const auto drop = add_function(module, {}, {
    bulk(9, std::uint32_t{1}),
    bulk(9, std::uint32_t{1}),
    i32(65'536), i32(0), i32(0), bulk(8, std::uint32_t{1}),
    i32(65'536), i32(65'536), i32(0), bulk(10),
    i32(65'536), i32(0), i32(0), bulk(11),
  });
  const auto use_dropped = add_function(module, {}, {
    i32(0), i32(0), i32(1), bulk(8, std::uint32_t{1}, 206),
  });
  const auto use_active = add_function(module, {}, {
    i32(0), i32(0), i32(1), bulk(8, std::uint32_t{0}, 207),
  });
  const auto past_end = add_function(module, {}, {
    i32(65'537), i32(0), i32(0), bulk(11, {}, 208),
  });
  const auto grow = add_function(module, {Type::i32, Type::i32, Type::i32, Type::i64, Type::i32}, {
    i32(1), op(0x40), op(0x3f),
    i32(0xffffffff), op(0x40), // Excessive growth fails without wrapping.
    i32(65'536), memory(0x29),
    i32(0), op(0x40),
  });

  wasm::Instance instance(std::move(module));
  check(instance.invoke(narrow) == std::vector<Value>{
      {Type::i64, 0xffffffffffffff80ULL}, {Type::i32, 0xff80}, {Type::f32, 0x7f800001}},
      "memory load sign-extension, little-endian order, or float payload was incorrect");
  check(instance.invoke(copy) == std::vector<Value>{{Type::i32, 0x07072305}},
        "memory bulk operations, overlapping copy, or fill byte truncation were incorrect");
  expect_trap([&] { instance.invoke(bad_store); }, 201, "out-of-bounds store did not trap");
  expect_trap([&] { instance.invoke(bad_fill); }, 202, "out-of-bounds fill did not trap");
  expect_trap([&] { instance.invoke(bad_copy); }, 203, "out-of-bounds copy did not trap");
  expect_trap([&] { instance.invoke(bad_init); }, 204, "out-of-bounds init did not trap");
  check(instance.invoke(last_byte) == std::vector<Value>{{Type::i32, 0x5a}},
        "failed store, fill, copy, or init partially wrote memory");
  expect_trap([&] { instance.invoke(bad_wrap); }, 205, "memory effective address wrapped to i32");
  instance.invoke(drop);
  expect_trap([&] { instance.invoke(use_dropped); }, 206, "memory.init used a dropped segment");
  expect_trap([&] { instance.invoke(use_active); }, 207, "active data segment was not dropped");
  expect_trap([&] { instance.invoke(past_end); }, 208, "zero-length fill allowed an address past the end");
  check(instance.invoke(grow) == std::vector<Value>{
      {Type::i32, 1}, {Type::i32, 2}, {Type::i32, 0xffffffff}, {Type::i64, 0}, {Type::i32, 2}},
      "memory.grow previous size, failure return, zeroing, or memory.size was incorrect");
}

void test_tables() {
  const auto ref = [](std::uint32_t function) { return index(0xd2, function); };
  const auto null = [](Type type) { return op(0xd0, type); };
  const auto pair = [](std::uint32_t first, std::uint32_t second) {
    return wasm::IndexPair{first, second};
  };
  wasm::Module module;
  const auto f0 = add_function(module, {}, {});
  const auto f1 = add_function(module, {}, {});
  module.tables = {
    {Type::funcref, {4, 5}}, {Type::funcref, {3, std::nullopt}}, {Type::externref, {2, 3}},
  };
  module.elements = {
    {wasm::SegmentMode::active, 0, {i32(0), op(0x0b)}, Type::funcref,
     {{ref(f0), op(0x0b)}, {ref(f1), op(0x0b)}}},
    {wasm::SegmentMode::passive, 0, {}, Type::funcref,
     {{ref(f1), op(0x0b)}, {ref(f0), op(0x0b)}}},
    {wasm::SegmentMode::declarative, 0, {}, Type::funcref, {{ref(f1), op(0x0b)}}},
  };
  const auto initialize = add_function(module, {Type::funcref, Type::i32, Type::funcref, Type::i32}, {
    i32(0), index(0x25, 0),
    i32(3), index(0x25, 0), op(0xd1),
    i32(2), i32(0), i32(2), bulk(12, pair(1, 0)), // table.init elem=1 table=0
    i32(2), index(0x25, 0),
    ref(f0), op(0xd1),
  });
  const auto copy = add_function(module, {Type::funcref, Type::funcref, Type::funcref, Type::funcref}, {
    i32(1), i32(0), i32(3), bulk(14, pair(0, 0)),
    i32(0), i32(1), i32(3), bulk(14, pair(0, 0)),
    i32(0), i32(1), i32(3), bulk(14, pair(1, 0)),
    i32(0), index(0x25, 0),
    i32(0), index(0x25, 1),
    i32(1), index(0x25, 1),
    i32(2), index(0x25, 1),
  });
  const auto grow = add_function(module, {Type::i32, Type::i32, Type::funcref, Type::i32}, {
    ref(f1), i32(1), bulk(15, std::uint32_t{0}),
    bulk(16, std::uint32_t{0}),
    i32(4), index(0x25, 0),
    null(Type::funcref), i32(1), bulk(15, std::uint32_t{0}),
  });
  const auto externs = add_function(module, {Type::i32, Type::externref}, {
    i32(0), index(0x20, 0), index(0x26, 2),
    i32(1), index(0x20, 0), i32(1), bulk(17, std::uint32_t{2}),
    i32(0), index(0x25, 2), op(0xd1),
    i32(1), index(0x25, 2),
  }, {Type::externref});
  const auto bad_set = add_function(module, {}, {
    i32(5), null(Type::funcref), op(0x26, std::uint32_t{0}, 301),
  });
  const auto bad_fill = add_function(module, {}, {
    i32(4), null(Type::funcref), i32(2), bulk(17, std::uint32_t{0}, 302),
  });
  const auto bad_copy = add_function(module, {}, {
    i32(4), i32(0), i32(2), bulk(14, pair(0, 1), 303),
  });
  const auto bad_init = add_function(module, {}, {
    i32(4), i32(0), i32(2), bulk(12, pair(1, 0), 304),
  });
  const auto at_end = add_function(module, {Type::funcref}, {
    i32(5), null(Type::funcref), i32(0), bulk(17, std::uint32_t{0}),
    i32(4), index(0x25, 0),
  });
  const auto past_end = add_function(module, {}, {
    i32(6), null(Type::funcref), i32(0), bulk(17, std::uint32_t{0}, 305),
  });
  const auto drop = add_function(module, {}, {
    bulk(13, std::uint32_t{1}),
    bulk(13, std::uint32_t{1}),
    i32(5), i32(0), i32(0), bulk(12, pair(1, 0)),
  });
  const auto use_dropped = add_function(module, {}, {
    i32(0), i32(0), i32(1), bulk(12, pair(1, 0), 306),
  });
  const auto use_active = add_function(module, {}, {
    i32(0), i32(0), i32(1), bulk(12, pair(0, 0), 307),
  });
  const auto use_declarative = add_function(module, {}, {
    i32(0), i32(0), i32(1), bulk(12, pair(2, 0), 308),
  });

  wasm::Instance instance(std::move(module));
  const Value first{Type::funcref, 1}, second{Type::funcref, 2};
  check(instance.invoke(initialize) == std::vector<Value>{
      first, {Type::i32, 1}, second, {Type::i32, 0}},
      "active/passive element initialization or ref.is_null was incorrect");
  check(instance.invoke(copy) == std::vector<Value>{first, second, second, second},
        "table.copy overlap or cross-table dispatch was incorrect");
  check(instance.invoke(grow) == std::vector<Value>{
      {Type::i32, 4}, {Type::i32, 5}, second, {Type::i32, 0xffffffff}},
      "table.grow previous size, failure return, initializer, or table.size was incorrect");
  const Value opaque{Type::externref, 0xff001234567890abULL};
  check(instance.invoke(externs, std::span(&opaque, 1)) == std::vector<Value>{{Type::i32, 0}, opaque},
        "externref table operations changed the opaque reference");
  expect_trap([&] { instance.invoke(bad_set); }, 301, "out-of-bounds table.set did not trap");
  expect_trap([&] { instance.invoke(bad_fill); }, 302, "out-of-bounds table.fill did not trap");
  expect_trap([&] { instance.invoke(bad_copy); }, 303, "out-of-bounds table.copy did not trap");
  expect_trap([&] { instance.invoke(bad_init); }, 304, "out-of-bounds table.init did not trap");
  expect_trap([&] { instance.invoke(past_end); }, 305, "zero-length fill allowed a table index past the end");
  check(instance.invoke(at_end) == std::vector<Value>{second},
        "failed table operation made a partial write");
  instance.invoke(drop);
  expect_trap([&] { instance.invoke(use_dropped); }, 306, "table.init used a dropped element");
  expect_trap([&] { instance.invoke(use_active); }, 307, "active element was not dropped");
  expect_trap([&] { instance.invoke(use_declarative); }, 308, "declarative element was not dropped");
}

void test_initialization() {
  wasm::Module module;
  module.memories.push_back({{0, std::nullopt}});
  module.tables.push_back({Type::externref, {0, std::nullopt}});
  module.data.push_back({wasm::SegmentMode::active, 0, {i32(0), op(0x0b)}, {}});
  module.elements.push_back({wasm::SegmentMode::active, 0, {i32(0), op(0x0b)}, Type::externref, {}});
  module.globals.push_back({{Type::i64, true}, {i64(42), op(0x0b)}});
  const wasm::VectorBytes pattern{
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
  };
  module.globals.push_back({{Type::v128, false}, {{{0xfd, 0x0c}, pattern, 0}, op(0x0b)}});
  const auto next = add_function(module, {Type::i64}, {
    index(0x23, 0), i64(1), op(0x7c), index(0x24, 0), index(0x23, 0),
  });
  const auto get_vector = add_function(module, {Type::v128}, {index(0x23, 1)});
  wasm::Instance instance(module);
  check(instance.invoke(next) == std::vector<Value>{{Type::i64, 43}} &&
        instance.invoke(next) == std::vector<Value>{{Type::i64, 44}},
        "global initializer or instance-global mutation was not applied");
  check(instance.invoke(get_vector) == std::vector<Value>{
      {Type::v128, 0xefcdab8967452301ULL, 0x1032547698badcfeULL}},
      "v128 global initializer did not preserve both little-endian halves");

  // Zero-length active segments do not waive the starting-offset check.
  module.data.front().offset.front() = op(0x41, std::uint32_t{1}, 401);
  expect_trap([&] { wasm::Instance invalid(module); }, 401,
              "empty active data segment past the end did not trap");
  module.elements.front().offset.front() = op(0x41, std::uint32_t{1}, 402);
  expect_trap([&] { wasm::Instance invalid(module); }, 402,
              "empty active element segment past the end did not trap");
}

} // namespace

int main() {
  try {
    test_integer();
    test_float();
    test_float_to_integer();
    test_memory();
    test_tables();
    test_initialization();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
