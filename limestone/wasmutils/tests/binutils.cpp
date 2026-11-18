#include "../WASM-Binutils.hpp"
#include "../../tests/test.hpp"

#include <array>

using namespace limestone::wasmutils;

namespace {

template<class F>
void fails_at(std::size_t offset, std::string_view explanation, F test) {
  try {
    test();
  } catch (const Error& error) {
    CHECK(error.offset == offset);
    const std::string_view message = error.what();
    CHECK(message.find("byte offset " + std::to_string(offset) + ":") !=
          std::string_view::npos);
    CHECK(message.find(explanation) != std::string_view::npos);
    return;
  }
  CHECK(false && "expected a WASM IR error");
}

void check_codecs() {
  for (const std::uint8_t opcode : {std::uint8_t{0x41}, std::uint8_t{0x42}}) {
    for (std::int32_t value = -64; value <= 63; ++value) {
      const std::vector<std::uint8_t> bytes{
          0x01, opcode, static_cast<std::uint8_t>(value < 0 ? value + 128 : value), 0x1a};
      const auto program = disassemble(bytes);
      CHECK(program.size() == 3);
      CHECK(program[1].opcode == opcode && program[1].immediate == value);
      CHECK(program[0].offset == 0 && program[1].offset == 1 && program[2].offset == 3);
      CHECK(assemble(program) == bytes);
    }
  }

  fails_at(19, "-64..63", [] { assemble(Program{{0x41, -65, 19}}); });
  fails_at(20, "-64..63", [] { assemble(Program{{0x42, 64, 20}}); });
  fails_at(14, "takes no immediate", [] { assemble(Program{{0x01, 1, 14}}); });
  fails_at(5, "unsupported opcode", [] { assemble(Program{{0x0b, 0, 5}}); });
  fails_at(1, "truncated", [] { disassemble(std::array<std::uint8_t, 2>{0x01, 0x41}); });
  fails_at(1, "continuation", [] {
    disassemble(std::array<std::uint8_t, 4>{0x01, 0x42, 0x80, 0x00});
  });
  fails_at(2, "unsupported opcode 0xfd", [] {
    disassemble(std::array<std::uint8_t, 3>{0x42, 0x7f, 0xfd});
  });

  // The raw codec accepts stack-invalid instructions; validate does not.
  const std::vector<std::uint8_t> add{0x6a};
  CHECK(assemble(disassemble(add)) == add);
  fails_at(0, "underflow", [&] { validate(disassemble(add)); });
}

void check_stack() {
  const auto result = validate(Program{
      {0x42, -64}, {0x50}, {0x41, 1}, {0x41, 0}, {0x1b}, {0x42, 63}});
  CHECK(result.results == std::vector<ValueType>{ValueType::i32, ValueType::i64});
  CHECK(result.max_stack == 3 && !result.always_traps);

  const auto trapped = validate(Program{
      {0x41, 1}, {0x42, 2}, {0x00}, {0x1b}, {0x50}, {0x1a}, {0x1a},
      {0x41, 0}, {0x41, 1}, {0x41, 2}, {0x00}, {0x8a}});
  CHECK(trapped.always_traps && trapped.results.empty());
  CHECK(trapped.max_stack == 2);

  // Concrete values may not be coerced, even when execution always traps.
  fails_at(3, "expected i32, found i64", [] {
    validate(Program{{0x00}, {0x42, 0}, {0x6a, 0, 3}});
  });
  fails_at(5, "select candidates must have the same type", [] {
    validate(Program{{0x00}, {0x42, 1}, {0x41, 2}, {0x41, 0}, {0x1b, 0, 5}});
  });
  fails_at(7, "expected i32, found i64", [] {
    validate(Program{{0x00}, {0x42, 0}, {0x1b, 0, 7}});
  });
  // One concrete and one polymorphic candidate yield the concrete type.
  fails_at(8, "expected i32, found i64", [] {
    validate(Program{{0x00}, {0x42, 1}, {0x41, 0}, {0x1b}, {0x45, 0, 8}});
  });
  // Typed instructions give concrete results even with polymorphic input.
  fails_at(9, "expected i64, found i32", [] {
    validate(Program{{0x00}, {0x1b}, {0x50}, {0x79, 0, 9}});
  });
  fails_at(10, "-64..63", [] { validate(Program{{0x00}, {0x41, 64, 10}}); });
}

void check_linker() {
  const std::uint8_t left[]{0x41, 0x40};
  const std::uint8_t right[]{0x41, 0x3f, 0x6a};
  const std::array<std::span<const std::uint8_t>, 3> fragments{left, {}, right};
  const auto linked = link(fragments);
  CHECK(linked == std::vector<std::uint8_t>{0x41, 0x40, 0x41, 0x3f, 0x6a});
  CHECK(validate(disassemble(linked)).results == std::vector<ValueType>{ValueType::i32});
  CHECK(link({}).empty());

  const std::uint8_t truncated[]{0x41};
  const std::uint8_t nop[]{0x01};
  fails_at(2, "truncated", [&] {
    link(std::array<std::span<const std::uint8_t>, 3>{left, truncated, nop});
  });
}

} // namespace

int main() {
  return test_main([] {
    check_codecs();
    check_stack();
    check_linker();
  });
}
