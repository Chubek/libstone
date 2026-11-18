#include "WASM-CC.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace limestone::wasmutils {
namespace {

constexpr std::string_view preamble = R"C(/* ANSI C89/C90 lowering of a WASM.aml opcode stream. */
#include <stddef.h>

struct wasm_result {
    unsigned width;
    unsigned long lo, hi;
};

/* Results are ordered bottom-to-top. Each limb contains 32 bits.
 * Returns 0 on success, 1 on trap, or 2 if the result buffer is too small
 * or missing. On failure no result is written. Optional count receives
 * the required result count, or zero on trap; it must not alias out.
 */
int wasm_run(struct wasm_result *out, size_t capacity, size_t *count);
)C";

constexpr std::string_view word_type = R"C(
struct wasm_word {
    unsigned long lo, hi;
};
)C";

constexpr std::string_view entry = R"C(
int wasm_run(struct wasm_result *out, size_t capacity, size_t *count)
{
)C";

enum class Helper : std::size_t {
    clz32, ctz32, popcnt32, sar32, rol32, add64, sub64, mul64,
    shl64, shr64, rol64, count
};

// All limb inputs are normalized to 32 bits. Unsigned long has at least
// 32 value bits in C89, and any wider intermediates are masked as needed.
// Helpers are listed in dependency order.
constexpr std::pair<Helper, std::string_view> helpers[] = {
    {Helper::clz32, R"C(
static unsigned long wasm_clz32(unsigned long a)
{
    unsigned long n = 0UL;
    unsigned long bit = 0x80000000UL;
    while (bit != 0UL && (a & bit) == 0UL) {
        ++n;
        bit >>= 1;
    }
    return n;
}
)C"},
    {Helper::ctz32, R"C(
static unsigned long wasm_ctz32(unsigned long a)
{
    unsigned long n = 0UL;
    if (a == 0UL)
        return 32UL;
    while ((a & 1UL) == 0UL) {
        ++n;
        a >>= 1;
    }
    return n;
}
)C"},
    {Helper::popcnt32, R"C(
static unsigned long wasm_popcnt32(unsigned long a)
{
    unsigned long n = 0UL;
    while (a != 0UL) {
        a &= a - 1UL;
        ++n;
    }
    return n;
}
)C"},
    {Helper::sar32, R"C(
static unsigned long wasm_sar32(unsigned long a, unsigned long n)
{
    unsigned long r = a >> n;
    if ((a & 0x80000000UL) != 0UL)
        r |= 0xffffffffUL ^ (0xffffffffUL >> n);
    return r;
}
)C"},
    {Helper::rol32, R"C(
static unsigned long wasm_rol32(unsigned long a, unsigned long n)
{
    return ((a << n) | (a >> ((0UL - n) & 31UL))) & 0xffffffffUL;
}
)C"},
    {Helper::add64, R"C(
static struct wasm_word wasm_add64(struct wasm_word a, struct wasm_word b)
{
    struct wasm_word r;
    r.lo = (a.lo + b.lo) & 0xffffffffUL;
    r.hi = (a.hi + b.hi + (r.lo < a.lo)) & 0xffffffffUL;
    return r;
}
)C"},
    {Helper::sub64, R"C(
static struct wasm_word wasm_sub64(struct wasm_word a, struct wasm_word b)
{
    struct wasm_word r;
    r.lo = (a.lo - b.lo) & 0xffffffffUL;
    r.hi = (a.hi - b.hi - (a.lo < b.lo)) & 0xffffffffUL;
    return r;
}
)C"},
    {Helper::mul64, R"C(
static struct wasm_word wasm_mul64(struct wasm_word a, struct wasm_word b)
{
    struct wasm_word r;
    unsigned long al = a.lo & 0xffffUL;
    unsigned long ah = a.lo >> 16;
    unsigned long bl = b.lo & 0xffffUL;
    unsigned long bh = b.lo >> 16;
    /* Split the low-by-low product into 16-bit digits. The first three
     * partial sums fit in 32 bits, including their incoming carry.
     */
    unsigned long p0 = al * bl;
    unsigned long p1 = ah * bl + (p0 >> 16);
    unsigned long p2 = al * bh + (p1 & 0xffffUL);
    r.lo = ((p2 << 16) & 0xffffffffUL) | (p0 & 0xffffUL);
    r.hi = (ah * bh + (p1 >> 16) + (p2 >> 16)
            + a.lo * b.hi + a.hi * b.lo) & 0xffffffffUL;
    return r;
}
)C"},
    {Helper::shl64, R"C(
static struct wasm_word wasm_shl64(struct wasm_word a, unsigned long n)
{
    struct wasm_word r;
    if (n == 0UL)
        return a;
    if (n < 32UL) {
        r.lo = (a.lo << n) & 0xffffffffUL;
        r.hi = ((a.hi << n) | (a.lo >> (32UL - n))) & 0xffffffffUL;
    } else {
        r.lo = 0UL;
        r.hi = (a.lo << (n - 32UL)) & 0xffffffffUL;
    }
    return r;
}
)C"},
    {Helper::shr64, R"C(
static struct wasm_word wasm_shr64(struct wasm_word a, unsigned long n,
                                   unsigned long fill)
{
    struct wasm_word r;
    if (n == 0UL)
        return a;
    if (n < 32UL) {
        r.lo = ((a.hi << (32UL - n)) | (a.lo >> n)) & 0xffffffffUL;
        r.hi = (a.hi >> n) | ((fill << (32UL - n)) & 0xffffffffUL);
    } else {
        n -= 32UL;
        r.lo = a.hi >> n;
        if (n != 0UL)
            r.lo |= (fill << (32UL - n)) & 0xffffffffUL;
        r.hi = fill;
    }
    return r;
}
)C"},
    {Helper::rol64, R"C(
static struct wasm_word wasm_rol64(struct wasm_word a, unsigned long n)
{
    struct wasm_word l = wasm_shl64(a, n);
    struct wasm_word r = wasm_shr64(a, (0UL - n) & 63UL, 0UL);
    l.lo |= r.lo;
    l.hi |= r.hi;
    return l;
}
)C"},
};

std::string slot(std::size_t index) {
    return "s[" + std::to_string(index) + "]";
}

std::string_view infix(Operation op) {
    switch (op) {
    case Operation::add: return "+";
    case Operation::sub: return "-";
    case Operation::mul: return "*";
    case Operation::bit_and: return "&";
    case Operation::bit_or: return "|";
    case Operation::bit_xor: return "^";
    default: return {};
    }
}

class Emitter {
public:
    Emitter() {
        body_.imbue(std::locale::classic());
        body_.exceptions(std::ios::badbit | std::ios::failbit);
    }

    std::string run(std::span<const Instruction> program, const Validation& validation) {
        const auto& results = validation.results;
        if (validation.always_traps || results.empty()) {
            // No selected operation has observable effects before a trap
            // or when every result is discarded.
            body_ << entry
                  << "    (void)out;\n"
                     "    (void)capacity;\n"
                     "    if (count != NULL)\n"
                     "        *count = 0;\n"
                     "    return " << (validation.always_traps ? 1 : 0) << ";\n"
                  << "}\n";
            return std::string(preamble) + body_.str();
        }

        body_ << entry << "    struct wasm_word s[" << validation.max_stack << "];\n"
              << "    if (count != NULL)\n"
              << "        *count = " << results.size() << ";\n"
              << "    if (out == NULL || capacity < " << results.size() << ")\n"
              << "        return 2;\n\n";
        for (const auto& instruction : program)
            emit(*opcode_info(instruction.opcode), instruction.immediate);
        body_ << '\n';
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto width = results[i] == ValueType::i32 ? 32 : 64;
            body_ << "    out[" << i << "].width = " << width << "U;\n"
                  << "    out[" << i << "].lo = " << slot(i) << ".lo;\n"
                  << "    out[" << i << "].hi = " << slot(i) << ".hi;\n";
        }
        body_ << "    return 0;\n}\n";

        std::string source(preamble);
        source += word_type;
        for (const auto& [helper, code] : helpers)
            if (used_[static_cast<std::size_t>(helper)])
                source += code;
        source += body_.str();
        return source;
    }

private:
    std::ostringstream body_;
    std::size_t depth_{};
    std::array<bool, static_cast<std::size_t>(Helper::count)> used_{};

    std::string_view use(Helper id, std::string_view name) {
        used_[static_cast<std::size_t>(id)] = true;
        return name;
    }

    void emit(const OpcodeInfo& op, std::int32_t immediate) {
        switch (op.operation) {
        case Operation::constant: {
            const auto target = slot(depth_++);
            const auto low = static_cast<std::uint32_t>(immediate);
            body_ << "    " << target << ".lo = " << low << "UL;\n"
                  << "    " << target << ".hi = "
                  << (op.width == 64 && immediate < 0 ? "0xffffffffUL" : "0UL") << ";\n";
            break;
        }
        case Operation::nop:
            break;
        case Operation::drop:
            --depth_;
            break;
        case Operation::select: {
            const auto a = slot(depth_ - 3);
            const auto b = slot(depth_ - 2);
            const auto c = slot(depth_ - 1);
            depth_ -= 2;
            body_ << "    " << a << " = " << c << ".lo != 0UL ? "
                  << a << " : " << b << ";\n";
            break;
        }
        case Operation::eqz:
        case Operation::clz:
        case Operation::ctz:
        case Operation::popcnt:
            unary(op);
            break;
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
        case Operation::rotr:
            binary(op);
            break;
        case Operation::unreachable:
            throw std::logic_error("unreachable instruction reached C lowering");
        }
    }

    void unary(const OpcodeInfo& op) {
        const auto a = slot(depth_ - 1);
        const auto lo = a + ".lo";
        const auto hi = a + ".hi";
        const bool wide = op.width == 64;
        body_ << "    " << lo << " = ";
        switch (op.operation) {
        case Operation::eqz:
            body_ << lo << " == 0UL";
            if (wide)
                body_ << " && " << hi << " == 0UL";
            break;
        case Operation::clz: {
            const auto clz = use(Helper::clz32, "wasm_clz32");
            if (wide)
                body_ << hi << " != 0UL ? " << clz << '(' << hi << ") : 32UL + ";
            body_ << clz << '(' << lo << ')';
            break;
        }
        case Operation::ctz: {
            const auto ctz = use(Helper::ctz32, "wasm_ctz32");
            body_ << ctz << '(' << lo << ')';
            if (wide)
                body_ << " + (" << lo << " == 0UL ? " << ctz << '(' << hi << ") : 0UL)";
            break;
        }
        case Operation::popcnt: {
            const auto popcnt = use(Helper::popcnt32, "wasm_popcnt32");
            body_ << popcnt << '(' << lo << ')';
            if (wide)
                body_ << " + " << popcnt << '(' << hi << ')';
            break;
        }
        default:
            throw std::logic_error("non-unary opcode reached unary C lowering");
        }
        body_ << ";\n";
        if (wide)
            body_ << "    " << hi << " = 0UL;\n";
    }

    void binary(const OpcodeInfo& op) {
        const auto a = slot(depth_ - 2);
        const auto b = slot(depth_ - 1);
        --depth_;
        if (op.width == 64)
            binary64(op.operation, a, b);
        else
            binary32(op.operation, a + ".lo", b + ".lo");
    }

    void binary32(Operation op, const std::string& lo, const std::string& rhs) {
        body_ << "    " << lo << " = ";
        if (const auto symbol = infix(op); !symbol.empty()) {
            body_ << '(' << lo << ' ' << symbol << ' ' << rhs << ") & 0xffffffffUL;\n";
            return;
        }
        const auto count = '(' + rhs + " & 31UL)";
        switch (op) {
        case Operation::shl:
            body_ << '(' << lo << " << " << count << ") & 0xffffffffUL";
            break;
        case Operation::shr_u:
            body_ << lo << " >> " << count;
            break;
        case Operation::shr_s:
            body_ << use(Helper::sar32, "wasm_sar32") << '(' << lo << ", " << count << ')';
            break;
        case Operation::rotl:
        case Operation::rotr:
            body_ << use(Helper::rol32, "wasm_rol32") << '(' << lo << ", "
                  << (op == Operation::rotl ? count : "((0UL - " + rhs + ") & 31UL)") << ')';
            break;
        default:
            throw std::logic_error("non-binary opcode reached i32 C lowering");
        }
        body_ << ";\n";
    }

    void binary64(Operation op, const std::string& a, const std::string& b) {
        const auto count = '(' + b + ".lo & 63UL)";
        switch (op) {
        case Operation::add:
        case Operation::sub:
        case Operation::mul: {
            const auto fn = op == Operation::add ? use(Helper::add64, "wasm_add64")
                          : op == Operation::sub ? use(Helper::sub64, "wasm_sub64")
                                                 : use(Helper::mul64, "wasm_mul64");
            body_ << "    " << a << " = " << fn << '(' << a << ", " << b << ");\n";
            break;
        }
        case Operation::bit_and:
        case Operation::bit_or:
        case Operation::bit_xor:
            body_ << "    " << a << ".lo " << infix(op) << "= " << b << ".lo;\n"
                  << "    " << a << ".hi " << infix(op) << "= " << b << ".hi;\n";
            break;
        case Operation::shl:
            body_ << "    " << a << " = " << use(Helper::shl64, "wasm_shl64")
                  << '(' << a << ", " << count << ");\n";
            break;
        case Operation::shr_s:
        case Operation::shr_u: {
            const auto fill = op == Operation::shr_s
                ? "(" + a + ".hi & 0x80000000UL) != 0UL ? 0xffffffffUL : 0UL"
                : "0UL";
            body_ << "    " << a << " = " << use(Helper::shr64, "wasm_shr64")
                  << '(' << a << ", " << count << ", " << fill << ");\n";
            break;
        }
        case Operation::rotl:
        case Operation::rotr:
            use(Helper::shl64, "wasm_shl64");
            use(Helper::shr64, "wasm_shr64");
            body_ << "    " << a << " = " << use(Helper::rol64, "wasm_rol64") << '(' << a << ", "
                  << (op == Operation::rotl ? count : "((0UL - " + b + ".lo) & 63UL)") << ");\n";
            break;
        default:
            throw std::logic_error("non-binary opcode reached i64 C lowering");
        }
    }
};

} // namespace

std::string emit_c(std::span<const Instruction> program) {
    const auto validation = validate(program);
    return Emitter{}.run(program, validation);
}

} // namespace limestone::wasmutils
