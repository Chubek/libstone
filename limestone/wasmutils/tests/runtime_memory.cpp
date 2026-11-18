#include "wasmutils/WASM-Memory.hpp"
#include "wasmutils/WASM-Runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace wasm = limestone::wasmutils;
namespace {

void check(bool condition, const char* description) {
    if (!condition)
        throw std::runtime_error(description);
}

template <class Exception, class Action>
void rejects(Action action, const char* description) {
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(description);
}

void test_memory() {
    wasm::LinearMemory memory(9);
    check(memory.size() == 9 && memory.load64(1) == 0, "zero-initialized memory");
    memory.store64(1, 0x0123456789abcdef);
    check(memory.load8(8) == 0x01 && memory.load16(1) == 0xcdef, "little-endian byte and short loads");
    check(memory.load32(2) == 0x6789abcd && memory.load64(1) == 0x0123456789abcdef,
          "unaligned little-endian word loads");
    memory.store8(8, 0xfe);
    memory.store16(2, 0x1234);
    memory.store32(4, 0x76543210);
    check(memory.load64(1) == 0xfe765432101234ef, "little-endian subword stores");

    rejects<std::out_of_range>([&] { memory.store64(2, 0); }, "store extending past the end");
    check(memory.load64(1) == 0xfe765432101234ef, "failed store partially wrote memory");
    rejects<std::out_of_range>([&] { (void)memory.load8(9); }, "read at end of memory");
    rejects<std::out_of_range>([&] { (void)memory.load32(std::numeric_limits<std::uint64_t>::max()); },
                               "effective address overflow");
    rejects<std::out_of_range>([&] { memory.store8(wasm::LinearMemory::address_space_size, 1); },
                               "address beyond wasm32");
    rejects<std::out_of_range>([] { (void)wasm::LinearMemory{}.load8(0); }, "read from empty memory");
    rejects<std::length_error>([] { wasm::LinearMemory too_large(wasm::LinearMemory::address_space_size + 1); },
                               "size beyond wasm32");
}

void expect_invalid(std::span<const wasm::Instruction> program, std::size_t offset) {
    try {
        (void)wasm::execute(program);
    } catch (const wasm::Error& error) {
        check(error.offset == offset, "wrong validation error offset");
        return;
    }
    throw std::runtime_error("execution accepted invalid IR");
}

void test_execution() {
    const std::array<wasm::Instruction, 5> valid{{
        {0x41, 4, 0}, {0x42, -1, 2}, {0x50, 0, 4}, {0x01, 0, 5}, {0x41, -1, 6},
    }};
    const auto result = wasm::execute(valid);
    using enum wasm::ValueType;
    check(!result.trapped && !result.trap_offset &&
          result.stack == std::vector<wasm::Value>{{i32, 4}, {i32, 0}, {i32, 0xffffffff}},
          "mixed operations, result order and width");

    const std::array<wasm::Instruction, 4> trap{{
        {0x41, 7, 0}, {0x00, 0, 2}, {0x8a, 0, 3}, {0x00, 0, 4},
    }};
    const auto trapped = wasm::execute(trap);
    check(trapped.trapped && trapped.stack.empty() && trapped.trap_offset == 2,
          "first trap offset and no fallthrough results");

    const std::array<wasm::Instruction, 1> underflow{{{0x1a, 0, 17}}};
    expect_invalid(underflow, 17);
    const std::array<wasm::Instruction, 2> unsupported_suffix{{{0x00, 0, 0}, {0x28, 0, 1}}};
    expect_invalid(unsupported_suffix, 1);
    const std::array<wasm::Instruction, 2> invalid_immediate{{{0x00, 0, 0}, {0x41, 64, 1}}};
    expect_invalid(invalid_immediate, 1);
    const std::array<wasm::Instruction, 3> wrong_suffix_type{{{0x00, 0, 0}, {0x42, 0, 1}, {0x45, 0, 3}}};
    expect_invalid(wrong_suffix_type, 3);
}

} // namespace

int main() {
    try {
        test_memory();
        test_execution();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
