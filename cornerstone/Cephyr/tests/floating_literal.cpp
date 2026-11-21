#include "../src/floating_literal.hpp"

#include <array>
#include <cfenv>
#include <cstdio>
#include <random>
#include <string>

using cephyr::detail::FloatingLiteralStatus;
using cephyr::detail::convert_floating_literal;
using cephyr::detail::floating_literal_to_integer;
using cephyr::detail::integer_to_floating_literal;
using cephyr::detail::parse_floating_literal;

int main() {
    struct Case {
        std::string_view spelling;
        unsigned width;
        std::uint64_t bits;
    };
    const std::array good = {
        Case{"0E+9999999999999999999", 64, 0},
        Case{"0X.0P-99999999999999999F", 32, 0},
        Case{".1f", 32, 0x3dcccccd},
        Case{"0.1", 64, UINT64_C(0x3fb999999999999a)},
        Case{"1.F", 32, 0x3f800000},
        Case{"0X.8P+1", 64, UINT64_C(0x3ff0000000000000)},
        Case{"1e1", 64, UINT64_C(0x4024000000000000)},
        Case{"000017.e-01f", 32, 0x3fd9999a},
        Case{"0x1.fffffep127f", 32, 0x7f7fffff},
        Case{"0x1.fffffffffffffp1023", 64, UINT64_C(0x7fefffffffffffff)},
        Case{"0x1p-126f", 32, 0x00800000},
        Case{"0x1p-149f", 32, 0x00000001},
        Case{"0x1p-1074", 64, 1},
        Case{"0x1p-150f", 32, 0},       // Exact midpoint rounds to even.
        Case{"0x1.000002p-150f", 32, 1}, // Just above the midpoint, still nonzero.
        Case{"0x1p-1075", 64, 0},
        Case{"0x1.0000000000001p-1075", 64, 1},
        Case{"1e-999999999999999999", 64, 0},
        Case{"0x0.0001p-99999999f", 32, 0},
        Case{"0.0000000000000000000000000000000000000000000567890f", 32, 41},
        Case{"0x1.000001p0f", 32, 0x3f800000},
        Case{"0x1.00000100000000000001p0f", 32, 0x3f800001},
        Case{"1.000000059604644775390625f", 32, 0x3f800000},
        Case{"1.000000059604644775390625000000000001f", 32, 0x3f800001},
        Case{"1.000000178813934326171875f", 32, 0x3f800002},
    };
    for (const auto &test : good) {
        const auto literal = parse_floating_literal(test.spelling);
        if (!literal || literal.width != test.width || literal.bits != test.bits) {
            std::fprintf(stderr, "%.*s: status=%d, width=%u, bits=%llx (want %u/%llx)\n",
                static_cast<int>(test.spelling.size()), test.spelling.data(),
                static_cast<int>(literal.status), literal.width,
                static_cast<unsigned long long>(literal.bits), test.width,
                static_cast<unsigned long long>(test.bits));
            return 1;
        }
    }
    for (std::string_view spelling : {"", ".", "1", "1f", "-1.0", "+1.0", "1,0", " 1.0",
            "1.0 ", "1.0.0", "1.0Lx", "1.e", "1.0e+", "0x", "0x1", "0x1.0", "0x1p",
            "0x1p-FF", "0x1p1.0", "0x.p1", "0x1p1FF", "inf", "NaN", "1.0ff"}) {
        if (parse_floating_literal(spelling).status != FloatingLiteralStatus::malformed) {
            std::fprintf(stderr, "accepted malformed spelling: %.*s\n",
                static_cast<int>(spelling.size()), spelling.data());
            return 2;
        }
    }
    if (parse_floating_literal("1.0l").status != FloatingLiteralStatus::unsupported_long_double ||
        parse_floating_literal("0XFF.P-1L").status != FloatingLiteralStatus::unsupported_long_double ||
        parse_floating_literal("0x1p128f").status != FloatingLiteralStatus::overflow ||
        parse_floating_literal("1e99999999999999").status != FloatingLiteralStatus::overflow ||
        parse_floating_literal(std::string(1024 * 1024 + 1, '0')).status !=
            FloatingLiteralStatus::too_long)
        return 3;
    if (convert_floating_literal(parse_floating_literal("1.000000059604644775390625000000000001"),
                                32).bits != 0x3f800000 ||
        convert_floating_literal(parse_floating_literal("0x1p-149f"), 64).bits != UINT64_C(0x36a0000000000000) ||
        convert_floating_literal(parse_floating_literal("0x1p-149"), 32).bits != 1 ||
        convert_floating_literal(parse_floating_literal("0x1p-150"), 32).bits != 0 ||
        convert_floating_literal(parse_floating_literal("0x1.000002p-150"), 32).bits != 1 ||
        convert_floating_literal(parse_floating_literal("0x1.ffffffp-127"), 32).bits != 0x00800000 ||
        convert_floating_literal(parse_floating_literal("0x1.fffffffffffffp127"), 32).status !=
            FloatingLiteralStatus::overflow ||
        convert_floating_literal({FloatingLiteralStatus::ok, 64, UINT64_C(0x8000000000000000)}, 32).bits != 0x80000000)
        return 4;
    if (integer_to_floating_literal(UINT64_MAX, true, 32).bits != 0xbf800000 ||
        integer_to_floating_literal(UINT64_MAX, false, 32).bits != 0x5f800000 ||
        integer_to_floating_literal(UINT64_MAX, false, 64).bits != UINT64_C(0x43f0000000000000) ||
        integer_to_floating_literal(UINT64_C(0x8000000000000000), true, 64).bits != UINT64_C(0xc3e0000000000000) ||
        integer_to_floating_literal(16777217, false, 32).bits != 0x4b800000 ||
        integer_to_floating_literal(16777219, false, 32).bits != 0x4b800002)
        return 5;
    const auto previous_rounding = std::fegetround();
    if (previous_rounding != -1 && std::fesetround(FE_UPWARD) == 0) {
        const auto tie = parse_floating_literal("1.000000059604644775390625f");
        const auto decimal = parse_floating_literal("0.1");
        std::fesetround(previous_rounding);
        if (tie.bits != 0x3f800000 || decimal.bits != UINT64_C(0x3fb999999999999a))
            return 6;
    }
    const auto negative_zero = cephyr::detail::FloatingLiteral{FloatingLiteralStatus::ok, 32, 0x80000000};
    const auto negative_half = cephyr::detail::FloatingLiteral{FloatingLiteralStatus::ok, 32, 0xbf000000};
    if (floating_literal_to_integer(negative_zero, 1, false) != 0 ||
        floating_literal_to_integer(negative_half, 1, false) != 1 ||
        floating_literal_to_integer(negative_half, 64, false) != 0 ||
        floating_literal_to_integer(parse_floating_literal("2147483647.99"), 32, true) != 2147483647 ||
        floating_literal_to_integer(parse_floating_literal("2147483648.0"), 32, true) ||
        floating_literal_to_integer(parse_floating_literal("0x1.fffffffffffffp63"), 64, false) !=
            UINT64_C(0xfffffffffffff800) ||
        floating_literal_to_integer(parse_floating_literal("0x1p64"), 64, false))
        return 7;

    // Differential checks use the test compiler only as an independent
    // execution oracle. Production lowering never invokes it.
    std::mt19937_64 rng(0x43453131);
    for (unsigned sample = 0; sample < 4096; ++sample) {
        const auto bits = rng();
        const auto signed_bits = std::bit_cast<std::int64_t>(bits);
        if (integer_to_floating_literal(bits, false, 32).bits != std::bit_cast<std::uint32_t>(float(bits)) ||
            integer_to_floating_literal(bits, false, 64).bits != std::bit_cast<std::uint64_t>(double(bits)) ||
            integer_to_floating_literal(bits, true, 32).bits != std::bit_cast<std::uint32_t>(float(signed_bits)) ||
            integer_to_floating_literal(bits, true, 64).bits != std::bit_cast<std::uint64_t>(double(signed_bits)))
            return 8;
        const auto raw_f32 = static_cast<std::uint32_t>(bits);
        if ((raw_f32 & 0x7f800000) != 0x7f800000) {
            const auto ref = std::bit_cast<std::uint64_t>(double(std::bit_cast<float>(raw_f32)));
            if (convert_floating_literal({FloatingLiteralStatus::ok, 32, raw_f32}, 64).bits != ref)
                return 9;
        }
        // Vary the exponent densely around overflow and subnormal boundaries.
        const auto raw_f64 = (bits & UINT64_C(0x800fffffffffffff)) |
            (UINT64_C(862) + (bits >> 54) % 289) << 52; // exponents -161..127
        const double real = std::bit_cast<double>(raw_f64);
        if (real <= std::numeric_limits<float>::max() && real >= -std::numeric_limits<float>::max()) {
            const auto ref = std::bit_cast<std::uint32_t>(float(real));
            if (convert_floating_literal({FloatingLiteralStatus::ok, 64, raw_f64}, 32).bits != ref)
                return 10;
        }
        if (real >= -0x1p63 && real < 0x1p63) {
            const auto ref = static_cast<std::uint64_t>(static_cast<std::int64_t>(real));
            if (floating_literal_to_integer({FloatingLiteralStatus::ok, 64, raw_f64}, 64, true) != ref)
                return 11;
        }
    }
}
