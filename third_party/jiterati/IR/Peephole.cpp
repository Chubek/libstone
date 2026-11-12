#include "IR.hpp"

#include <cstdint>
#include <optional>

namespace jiterati::ir {
namespace {

std::optional<std::int64_t> as_i64(TACValue const& value) {
    if (value.kind != TACValue::Kind::Constant) return std::nullopt;
    if (auto const* integer = std::get_if<std::int64_t>(&value.constant)) return *integer;
    if (auto const* boolean = std::get_if<bool>(&value.constant)) return *boolean ? 1 : 0;
    return std::nullopt;
}

unsigned integer_width(Type type) {
    if (type == Type::bool_ty()) return 1;
    if (type == Type::i32()) return 32;
    if (type == Type::i64()) return 64;
    return 0;
}

std::uint64_t mask_for(unsigned width) {
    return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}

std::int64_t signed_value(std::uint64_t value, unsigned width) {
    auto mask = mask_for(width);
    value &= mask;
    if (!(value & (UINT64_C(1) << (width - 1)))) return static_cast<std::int64_t>(value);
    return -1 - static_cast<std::int64_t>((~value) & mask);
}

std::optional<std::int64_t> fold_integer(TACOpcode opcode, std::int64_t left,
                                       std::int64_t right, unsigned width) {
    auto mask = mask_for(width);
    auto lhs = static_cast<std::uint64_t>(left) & mask;
    auto rhs = static_cast<std::uint64_t>(right) & mask;
    auto sa = signed_value(lhs, width), sb = signed_value(rhs, width);
    auto minimum = signed_value(UINT64_C(1) << (width - 1), width);
    bool overflow = sa == minimum && sb == -1;
    std::uint64_t result;
    // Fixed-width semantics, independent of the compiler host. As in gas's
    // expression folding, shifts use unsigned intermediates and checked counts.
    switch (opcode) {
        case TACOpcode::Add: result = lhs + rhs; break;
        case TACOpcode::Sub: result = lhs - rhs; break;
        case TACOpcode::Mul: result = lhs * rhs; break;
        case TACOpcode::SDiv: if (!rhs || overflow) return {}; result = static_cast<std::uint64_t>(sa / sb); break;
        case TACOpcode::SRem: if (!rhs || overflow) return {}; result = static_cast<std::uint64_t>(sa % sb); break;
        case TACOpcode::UDiv: if (!rhs) return {}; result = lhs / rhs; break;
        case TACOpcode::URem: if (!rhs) return {}; result = lhs % rhs; break;
        case TACOpcode::And: result = lhs & rhs; break;
        case TACOpcode::Or: result = lhs | rhs; break;
        case TACOpcode::Xor: result = lhs ^ rhs; break;
        case TACOpcode::Neg: result = UINT64_C(0) - lhs; break;
        case TACOpcode::Not: result = ~lhs; break;
        case TACOpcode::Shl: if (rhs >= width) return {}; result = lhs << rhs; break;
        case TACOpcode::LShr: if (rhs >= width) return {}; result = lhs >> rhs; break;
        case TACOpcode::AShr:
            if (rhs >= width) return {};
            result = lhs >> rhs;
            if (sa < 0 && rhs) result |= mask ^ (mask >> rhs);
            break;
        case TACOpcode::Eq: return lhs == rhs ? 1 : 0;
        case TACOpcode::Ne: return lhs != rhs ? 1 : 0;
        case TACOpcode::SLt: return sa < sb ? 1 : 0;
        case TACOpcode::SLe: return sa <= sb ? 1 : 0;
        case TACOpcode::SGt: return sa > sb ? 1 : 0;
        case TACOpcode::SGe: return sa >= sb ? 1 : 0;
        case TACOpcode::ULt: return lhs < rhs ? 1 : 0;
        case TACOpcode::ULe: return lhs <= rhs ? 1 : 0;
        case TACOpcode::UGt: return lhs > rhs ? 1 : 0;
        case TACOpcode::UGe: return lhs >= rhs ? 1 : 0;
        default: return {};
    }
    return signed_value(result, width);
}

bool is_binary(TACOpcode opcode) {
    switch (opcode) {
        case TACOpcode::Add:
        case TACOpcode::Sub:
        case TACOpcode::Mul:
        case TACOpcode::SDiv:
        case TACOpcode::UDiv:
        case TACOpcode::SRem:
        case TACOpcode::URem:
        case TACOpcode::And:
        case TACOpcode::Or:
        case TACOpcode::Xor:
        case TACOpcode::Shl:
        case TACOpcode::LShr:
        case TACOpcode::AShr:
        case TACOpcode::Eq:
        case TACOpcode::Ne:
        case TACOpcode::SLt:
        case TACOpcode::SLe:
        case TACOpcode::SGt:
        case TACOpcode::SGe:
        case TACOpcode::ULt:
        case TACOpcode::ULe:
        case TACOpcode::UGt:
        case TACOpcode::UGe:
            return true;
        default:
            return false;
    }
}

} // namespace

bool peephole(TACFunction& function) {
    bool changed = false;
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            bool unary = instruction.opcode == TACOpcode::Neg || instruction.opcode == TACOpcode::Not;
            bool binary = is_binary(instruction.opcode);
            if (instruction.result.has_value() && (unary || binary) &&
                instruction.operands.size() == (unary ? 1u : 2u)) {
                unsigned width = integer_width(instruction.operands[0].type);
                if (!width || !integer_width(instruction.type) ||
                    (binary && instruction.operands[1].type != instruction.operands[0].type)) continue;
                auto lhs = as_i64(instruction.operands[0]);
                auto rhs = unary ? std::optional<std::int64_t>(0) : as_i64(instruction.operands[1]);
                if (lhs.has_value() && rhs.has_value()) {
                    auto folded = fold_integer(instruction.opcode, *lhs, *rhs, width);
                    if (folded.has_value()) {
                        Immediate value = instruction.type == Type::bool_ty() ? Immediate(*folded != 0) : Immediate(*folded);
                        TACValue replacement = TACValue::constant_value(instruction.type, value);
                        rewrite_values(function, *instruction.result, replacement);
                        instruction.opcode = TACOpcode::Move;
                        instruction.operands = { replacement };
                        changed = true;
                    }
                }
            }
            if (instruction.opcode == TACOpcode::Select && instruction.operands.size() == 3) {
                if (auto cond = as_i64(instruction.operands[0]); cond.has_value()) {
                    TACValue replacement = *cond ? instruction.operands[1] : instruction.operands[2];
                    if (instruction.result.has_value()) rewrite_values(function, *instruction.result, replacement);
                    instruction.opcode = TACOpcode::Move;
                    instruction.operands = { replacement };
                    changed = true;
                }
            }
        }
    }
    return changed;
}

bool peephole(TACModule& module) {
    bool changed = false;
    for (auto& function : module.functions) changed = peephole(function) || changed;
    return changed;
}

} // namespace jiterati::ir
