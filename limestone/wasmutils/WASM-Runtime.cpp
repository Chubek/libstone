#include "WASM-Runtime.hpp"

#include <bit>
#include <limits>
#include <stdexcept>

namespace limestone::wasmutils {
namespace {

template <typename Word>
std::uint64_t count_bits(Operation op, Word bits) {
    switch (op) {
    case Operation::clz:
        return static_cast<std::uint64_t>(std::countl_zero(bits));
    case Operation::ctz:
        return static_cast<std::uint64_t>(std::countr_zero(bits));
    case Operation::popcnt:
        return static_cast<std::uint64_t>(std::popcount(bits));
    default:
        throw std::logic_error("not a WASM bit-count operation");
    }
}

template <typename Word>
Word binary(Operation op, std::uint64_t lhs, std::uint64_t rhs) {
    // Evaluate in an explicitly unsigned type not subject to signed promotion.
    // Conversion back to Word keeps exactly the low 32 or 64 bits.
    const auto a = std::uintmax_t{lhs};
    const auto b = std::uintmax_t{rhs};
    constexpr auto mask = std::uintmax_t{std::numeric_limits<Word>::max()};
    constexpr auto width = std::numeric_limits<Word>::digits;
    const auto shift = static_cast<int>(b & (width - 1));
    switch (op) {
    case Operation::add:
        return static_cast<Word>(a + b);
    case Operation::sub:
        return static_cast<Word>(a - b);
    case Operation::mul:
        return static_cast<Word>(a * b);
    case Operation::bit_and:
        return static_cast<Word>(a & b);
    case Operation::bit_or:
        return static_cast<Word>(a | b);
    case Operation::bit_xor:
        return static_cast<Word>(a ^ b);
    case Operation::shl:
        return static_cast<Word>(a << shift);
    case Operation::shr_s:
        // For a negative word, complement before and after shifting to fill
        // with ones. Keep complements confined to the word width.
        return static_cast<Word>(a > (mask >> 1) ? mask ^ ((mask ^ a) >> shift) : a >> shift);
    case Operation::shr_u:
        return static_cast<Word>(a >> shift);
    case Operation::rotl:
        return std::rotl(static_cast<Word>(a), shift);
    case Operation::rotr:
        return std::rotr(static_cast<Word>(a), shift);
    default:
        throw std::logic_error("not a WASM binary operation");
    }
}

Value pop(std::vector<Value>& stack) {
    const auto value = stack.back();
    stack.pop_back();
    return value;
}

} // namespace

Execution execute(std::span<const Instruction> program) {
    const auto validated = validate(program);
    Execution result;
    auto& stack = result.stack;
    stack.reserve(validated.max_stack);

    for (const auto& instruction : program) {
        const auto& info = *opcode_info(instruction.opcode);
        switch (info.operation) {
        case Operation::unreachable:
            return {{}, true, instruction.offset};
        case Operation::nop:
            break;
        case Operation::drop:
            stack.pop_back();
            break;
        case Operation::select: {
            const auto condition = pop(stack);
            const auto otherwise = pop(stack);
            if (condition.bits == 0)
                stack.back() = otherwise;
            break;
        }
        case Operation::constant:
            if (info.width == 32)
                stack.push_back({ValueType::i32, static_cast<std::uint32_t>(instruction.immediate)});
            else
                stack.push_back({ValueType::i64, static_cast<std::uint64_t>(instruction.immediate)});
            break;
        case Operation::eqz:
            stack.back() = {ValueType::i32, stack.back().bits == 0 ? 1u : 0u};
            break;
        case Operation::clz:
        case Operation::ctz:
        case Operation::popcnt: {
            auto& value = stack.back();
            value.bits = value.type == ValueType::i32
                ? count_bits(info.operation, static_cast<std::uint32_t>(value.bits))
                : count_bits(info.operation, value.bits);
            break;
        }
        case Operation::add:
        case Operation::sub:
        case Operation::mul:
        case Operation::bit_and:
        case Operation::bit_or:
        case Operation::bit_xor:
        case Operation::shl:
        case Operation::shr_s:
        case Operation::shr_u:
        case Operation::rotl:
        case Operation::rotr: {
            const auto rhs = pop(stack);
            auto& lhs = stack.back();
            lhs.bits = lhs.type == ValueType::i32
                ? binary<std::uint32_t>(info.operation, lhs.bits, rhs.bits)
                : binary<std::uint64_t>(info.operation, lhs.bits, rhs.bits);
            break;
        }
        }
    }
    return result;
}

} // namespace limestone::wasmutils
