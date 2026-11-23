// Compiler-side finite constant policy around the same integer-only
// arithmetic implementation used by the target's soft floating ABI.
#include "floating_arithmetic.hpp"
#include "../stdlib/src/real_bits.h"

namespace cephyr::detail {
namespace {
FloatingLiteral checked(unsigned width, cephyr_real_bits bits) {
    const bool overflow = width == 128 ? ((bits.hi >> 48) & 0x7fff) == 0x7fff :
                          width == 80 ? (bits.hi & 0x7fff) == 0x7fff :
                          width == 64 ? ((bits.lo >> 52) & 0x7ff) == 0x7ff :
                                        ((bits.lo >> 23) & 255) == 255;
    return {overflow ? FloatingLiteralStatus::overflow : FloatingLiteralStatus::ok,
            width, bits.lo, bits.hi};
}
}
FloatingLiteral convert_quad_format(FloatingLiteral value, unsigned width) {
    if (!value || (width != 32 && width != 64 && width != 80 && width != 128)) return {};
    return checked(width, cephyr_real_cast(width, value.width, {value.bits, value.high_word}));
}
FloatingLiteral fold_quad(FloatingOperation op, FloatingLiteral lhs, FloatingLiteral rhs) {
    if (!lhs || !rhs) return {};
    const char code = op == FloatingOperation::add ? '+' : op == FloatingOperation::subtract ? '-' :
                      op == FloatingOperation::multiply ? '*' : '/';
    if (code == '/' && !rhs.bits && !(rhs.high_word << 1)) return {};
    return checked(128, cephyr_real_calc(128, code, {lhs.bits,lhs.high_word}, {rhs.bits,rhs.high_word}));
}
} // namespace cephyr::detail
