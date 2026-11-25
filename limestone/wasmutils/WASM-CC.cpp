#include "WASM-CC.hpp"
#include "WASM-Validate.hpp"
#include "backend/WASM-CC-Float.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <locale>
#include <map>
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

    std::string run_module(const core::Module& module);

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

    void module_opcode(const core::Instruction& insn);
    bool memory_access_{}, memory_grow_{}, fp_{};

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

constexpr std::string_view module_preamble = R"C(/* Standalone ANSI C90 lowering of a Wasm module. */
#include <stddef.h>

struct wasm_result {
    unsigned width;
    unsigned long lo, hi;
};

enum wasm_status {
    WASM_OK = 0, WASM_TRAP_UNREACHABLE = 1, WASM_OUTPUT_TOO_SMALL = 2,
    WASM_INVALID_ARGUMENT = 3, WASM_TRAP_INTEGER_DIVIDE_BY_ZERO = 4,
    WASM_TRAP_INTEGER_OVERFLOW = 5, WASM_TRAP_MEMORY_BOUNDS = 6,
    WASM_TRAP_FUEL = 7, WASM_TRAP_RESOURCE = 8,
    WASM_TRAP_INVALID_CONVERSION = 9, WASM_UNSUPPORTED_HOST = 10,
    WASM_TRAP_TABLE_BOUNDS = 11, WASM_TRAP_NULL_FUNCTION = 12,
    WASM_TRAP_INDIRECT_TYPE = 13, WASM_TRAP_HOST = 14, WASM_MISSING_IMPORT = 15
};

struct wasm_instance;

/* A host callback returns zero on success and supplies exactly nresults
 * raw limb values of the declared result widths. Any nonzero return traps.
 */
typedef int (*wasm_host_callback)(void *context, const struct wasm_result *args,
                                  size_t nargs, struct wasm_result *results, size_t nresults);
struct wasm_host_import {
    const char *module, *name;
    size_t module_size, name_size;
    wasm_host_callback callback;
    void *context;
};

/* Values use 32-bit lo/hi limbs, with hi zero for width 32.
 * Floating-point values use IEEE bits. References use width 64:
 * zero is null, funcref is a function index plus one in this instance,
 * and externref is an opaque bit pattern supplied by the host.
 * count must not alias args/out; args and out may overlap. A call executes
 * even if the result buffer is too small. Traps do not roll back state.
 * An instance is not thread-safe. Recursive invocation from its host
 * callbacks is not supported. Names are length-delimited UTF-8 bytes.
 */
/* Creation leaves *out NULL on failure and charges start_fuel only to start.
 * The instance keeps callbacks and contexts, not their name strings.
 */
int wasm_instance_create(struct wasm_instance **out, unsigned long start_fuel);
int wasm_instance_create_with_imports(struct wasm_instance **out,
                                      const struct wasm_host_import *imports, size_t nimports,
                                      unsigned long start_fuel);
void wasm_instance_destroy(struct wasm_instance *instance);
int wasm_instance_invoke(struct wasm_instance *instance,
                         const char *name, size_t name_size,
                         const struct wasm_result *args, size_t nargs,
                         struct wasm_result *out, size_t capacity, size_t *count,
                         unsigned long fuel);
int wasm_instance_read_memory(struct wasm_instance *instance,
                              const char *name, size_t name_size,
                              unsigned long offset, unsigned char *out, size_t size);
int wasm_instance_write_memory(struct wasm_instance *instance,
                               const char *name, size_t name_size,
                               unsigned long offset, const unsigned char *data, size_t size);
int wasm_instance_get_global(struct wasm_instance *instance,
                             const char *name, size_t name_size, struct wasm_result *out);
int wasm_instance_set_global(struct wasm_instance *instance,
                             const char *name, size_t name_size, const struct wasm_result *value);
int wasm_instance_get_table(struct wasm_instance *instance, const char *name, size_t name_size,
                            unsigned long index, struct wasm_result *out);
int wasm_instance_set_table(struct wasm_instance *instance, const char *name, size_t name_size,
                            unsigned long index, const struct wasm_result *value);

/* Legacy stateless wrapper: creates a fresh instance and collapses traps
 * and resource/host failures to 1. For persistent state use the API above.
 */
int wasm_module_invoke(const char *name, size_t name_size,
                       const struct wasm_result *args, size_t nargs,
                       struct wasm_result *out, size_t capacity, size_t *count,
                       unsigned long fuel);
)C";

constexpr std::string_view module_signature = R"C((const char *name, size_t name_size,
                       const struct wasm_result *args, size_t nargs,
                       struct wasm_result *out, size_t capacity, size_t *count,
                       unsigned long fuel)
{
)C";

constexpr std::string_view module_support = R"C(
#include <stdlib.h>
#include <string.h>

typedef int (*wasm_func)(struct wasm_instance *, struct wasm_word *, const struct wasm_word *,
                         unsigned long *, unsigned);
)C";
constexpr std::string_view module_alloc = R"C(
static struct wasm_word *wasm_alloc(unsigned long n)
{
    if (n == 0UL)
        n = 1UL;
    if (n > (size_t)-1 / sizeof(struct wasm_word))
        return NULL;
    return (struct wasm_word *)calloc((size_t)n, sizeof(struct wasm_word));
}
)C";

constexpr std::string_view module_imports = R"C(
struct wasm_binding {
    wasm_host_callback callback;
    void *context;
};

struct wasm_import_desc {
    const unsigned char *module, *name, *params, *results;
    size_t module_size, name_size, nargs, nresults;
};

static unsigned wasm_width(unsigned type);
static int wasm_valid_word(const struct wasm_result *v, unsigned type);

static int wasm_call_import(const struct wasm_binding *binding, const struct wasm_import_desc *desc,
                            struct wasm_word *out, const struct wasm_word *args,
                            unsigned long *fuel, unsigned depth)
{
    struct wasm_result *raw, *results;
    size_t i, n = desc->nargs + desc->nresults;
    if (depth == 0U) {
        if (*fuel == 0UL)
            return WASM_TRAP_FUEL;
        --*fuel;
    }
    if (n == 0)
        n = 1;
    if (n > (size_t)-1 / sizeof(*raw))
        return WASM_TRAP_RESOURCE;
    raw = (struct wasm_result *)calloc(n, sizeof(*raw));
    if (raw == NULL)
        return WASM_TRAP_RESOURCE;
    for (i = 0; i < desc->nargs; ++i) {
        raw[i].width = wasm_width((unsigned)desc->params[i]);
        raw[i].lo = args[i].lo;
        raw[i].hi = args[i].hi;
    }
    results = raw + desc->nargs;
    if (binding->callback(binding->context, raw, desc->nargs, results, desc->nresults) != 0) {
        free(raw);
        return WASM_TRAP_HOST;
    }
    for (i = 0; i < desc->nresults; ++i)
        if (!wasm_valid_word(&results[i], (unsigned)desc->results[i])) {
            free(raw);
            return WASM_TRAP_HOST;
        }
    for (i = 0; i < desc->nresults; ++i) {
        out[i].lo = results[i].lo;
        out[i].hi = results[i].hi;
    }
    free(raw);
    return WASM_OK;
}

static int wasm_same_name(const void *a, size_t na, const void *b, size_t nb)
{
    return na == nb && memcmp(a, b, na) == 0;
}
)C";

constexpr std::string_view module_memory = R"C(
struct wasm_memory {
    unsigned char *bytes;
    size_t size;
    unsigned long pages;
};
)C";

constexpr std::string_view module_memory_bounds = R"C(
static int wasm_memory_range(const struct wasm_memory *m, unsigned long address,
                             unsigned long offset, size_t size, size_t *location)
{
    if (address > m->size || offset > m->size - (size_t)address ||
        size > m->size - (size_t)address - (size_t)offset)
        return WASM_TRAP_MEMORY_BOUNDS;
    *location = (size_t)address + (size_t)offset;
    return WASM_OK;
}
)C";

constexpr std::string_view module_memory_access = R"C(
static int wasm_memory_access(struct wasm_memory *m, struct wasm_word *value,
                              unsigned long address, unsigned long offset,
                              unsigned n, int store, int sign, int wide)
{
    size_t pos;
    unsigned i;
    int status = wasm_memory_range(m, address, offset, n, &pos);
    if (status)
        return status;
    if (!store)
        value->lo = value->hi = 0UL;
    for (i = 0; i < n; ++i) {
        unsigned long *word = i < 4U ? &value->lo : &value->hi;
        unsigned shift = (i % 4U) * 8U;
        if (store)
            m->bytes[pos + i] = (unsigned char)((*word >> shift) & 0xffUL);
        else
            *word |= (unsigned long)m->bytes[pos + i] << shift;
    }
    if (!store && sign && (m->bytes[pos + n - 1U] & 0x80U)) {
        if (n < 4U)
            value->lo |= (0xffffffffUL << (n * 8U)) & 0xffffffffUL;
        if (wide)
            value->hi = 0xffffffffUL;
    }
    return WASM_OK;
}
)C";

constexpr std::string_view module_memory_grow = R"C(
static unsigned long wasm_memory_grow(struct wasm_memory *m, unsigned long delta,
                                      unsigned long max)
{
    unsigned long old = m->pages;
    unsigned long pages;
    size_t size;
    unsigned char *bytes;
    if (delta == 0UL)
        return old;
    if (delta > max - old)
        return 0xffffffffUL;
    pages = old + delta;
    if (pages > (size_t)-1 / 65536UL)
        return 0xffffffffUL;
    size = (size_t)pages * 65536UL;
    bytes = (unsigned char *)realloc(m->bytes, size);
    if (bytes == NULL)
        return 0xffffffffUL;
    memset(bytes + m->size, 0, size - m->size);
    m->bytes = bytes;
    m->size = size;
    m->pages = pages;
    return old;
}
)C";

constexpr std::string_view module_data_init = R"C(
static int wasm_memory_init(struct wasm_memory *m, const unsigned char *data,
                            size_t remaining, unsigned long dst,
                            unsigned long src, unsigned long n)
{
    size_t pos;
    if (src > remaining || n > remaining - (size_t)src ||
        wasm_memory_range(m, dst, 0UL, (size_t)n, &pos))
        return WASM_TRAP_MEMORY_BOUNDS;
    if (n != 0UL)
        memcpy(m->bytes + pos, data + (size_t)src, (size_t)n);
    return WASM_OK;
}
)C";

constexpr std::string_view module_table = R"C(
struct wasm_table {
    struct wasm_word *values;
    unsigned long size;
};
)C";

constexpr std::string_view module_table_grow = R"C(
static unsigned long wasm_table_grow(struct wasm_table *table, unsigned long delta,
                                     unsigned long limit, struct wasm_word value)
{
    unsigned long old = table->size, i;
    struct wasm_word *values;
    if (delta == 0UL)
        return old;
    if (delta > limit - old || old + delta > (size_t)-1 / sizeof(*values))
        return 0xffffffffUL;
    values = (struct wasm_word *)realloc(table->values, (size_t)(old + delta) * sizeof(*values));
    if (values == NULL)
        return 0xffffffffUL;
    for (i = old; i < old + delta; ++i)
        values[i] = value;
    table->values = values;
    table->size = old + delta;
    return old;
}
)C";

constexpr std::string_view module_table_init = R"C(
static int wasm_table_init(struct wasm_table *table, const struct wasm_word *values,
                           unsigned long remaining, unsigned long dst,
                           unsigned long src, unsigned long n)
{
    if (src > remaining || n > remaining - src || dst > table->size || n > table->size - dst)
        return WASM_TRAP_TABLE_BOUNDS;
    if (n != 0UL)
        memcpy(table->values + dst, values + src, (size_t)n * sizeof(*values));
    return WASM_OK;
}
)C";

constexpr std::string_view module_compare = R"C(
static int wasm_cmp(struct wasm_word a, struct wasm_word b, int sign, int wide)
{
    if (sign) {
        if (wide) {
            a.hi ^= 0x80000000UL;
            b.hi ^= 0x80000000UL;
        } else {
            a.lo ^= 0x80000000UL;
            b.lo ^= 0x80000000UL;
        }
    }
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo)
        return a.lo < b.lo ? -1 : 1;
    return 0;
}
)C";

constexpr std::string_view module_division = R"C(
static struct wasm_word wasm_negate(struct wasm_word a, int wide)
{
    struct wasm_word zero;
    zero.lo = zero.hi = 0UL;
    a = wasm_sub64(zero, a);
    if (!wide)
        a.hi = 0UL;
    return a;
}

static int wasm_divide(struct wasm_word *out, struct wasm_word a,
                       struct wasm_word b, int sign, int rem, int wide)
{
    struct wasm_word r, q;
    int n;
    int aneg = sign && ((wide ? a.hi : a.lo) & 0x80000000UL) != 0UL;
    int bneg = sign && ((wide ? b.hi : b.lo) & 0x80000000UL) != 0UL;
    if ((b.lo | b.hi) == 0UL)
        return WASM_TRAP_INTEGER_DIVIDE_BY_ZERO;
    if (aneg)
        a = wasm_negate(a, wide);
    if (bneg)
        b = wasm_negate(b, wide);
    if (!rem && aneg && bneg && b.lo == 1UL && b.hi == 0UL &&
        (wide ? a.hi == 0x80000000UL && a.lo == 0UL : a.lo == 0x80000000UL))
        return WASM_TRAP_INTEGER_OVERFLOW;
    r.lo = r.hi = q.lo = q.hi = 0UL;
    for (n = wide ? 64 : 32; n != 0; ) {
        unsigned long bit;
        --n;
        bit = n >= 32 ? (a.hi >> (n - 32)) & 1UL : (a.lo >> n) & 1UL;
        r = wasm_shl64(r, 1UL);
        r.lo |= bit;
        if (wasm_cmp(r, b, 0, wide) >= 0) {
            r = wasm_sub64(r, b);
            if (n >= 32)
                q.hi |= 1UL << (n - 32);
            else
                q.lo |= 1UL << n;
        }
    }
    if (rem)
        *out = aneg ? wasm_negate(r, wide) : r;
    else
        *out = aneg != bneg ? wasm_negate(q, wide) : q;
    return 0;
}
)C";

bool is_word(core::Type type) {
    using enum core::Type;
    return type != v128;
}

std::uint64_t constant_bits(const core::Instruction& insn) {
    if (insn.opcode.byte == 0xd0)
        return 0;
    if (insn.opcode.byte == 0xd2)
        return std::uint64_t{std::get<std::uint32_t>(insn.immediate)} + 1;
    if (const auto* low = std::get_if<std::uint32_t>(&insn.immediate))
        return *low;
    return std::get<std::uint64_t>(insn.immediate);
}

void emit_bits(std::ostream& out, std::string_view dest, std::uint64_t bits) {
    out << "    " << dest << ".lo = " << static_cast<std::uint32_t>(bits) << "UL;\n"
        << "    " << dest << ".hi = " << static_cast<std::uint32_t>(bits >> 32) << "UL;\n";
}

void Emitter::module_opcode(const core::Instruction& insn) {
    const auto code = insn.opcode.byte;
    const auto enable_fp = [&] {
        fp_ = true;
        use(Helper::add64, "wasm_add64");
        use(Helper::shl64, "wasm_shl64");
        use(Helper::shr64, "wasm_shr64");
    };
    const auto fp_trunc = [&](unsigned kind, bool saturating) {
        enable_fp();
        body_ << "    status = wasm_fp_to_integer(&stack[sp - 1], " << ((kind & 2U) != 0)
              << ", " << ((kind & 4U) != 0) << ", " << ((kind & 1U) == 0)
              << ", " << saturating << ");\n"
                 "    if (status)\n        goto wasm_fail;\n";
    };
    if (insn.opcode.extension == 0) {
        if (code >= 0x41 && code <= 0x44) {
            emit_bits(body_, "stack[sp]", constant_bits(insn));
            body_ << "    ++sp;\n";
            return;
        }
        if (code == 0xd0 || code == 0xd2) {
            emit_bits(body_, "stack[sp]", constant_bits(insn));
            body_ << "    ++sp;\n";
            return;
        }
        if (code == 0xd1) {
            body_ << "    stack[sp - 1].lo = (stack[sp - 1].lo | stack[sp - 1].hi) == 0UL;\n"
                     "    stack[sp - 1].hi = 0UL;\n";
            return;
        }
        if (code == 0x1c || code == 0x1b) {
            body_ << "    if (stack[sp - 1].lo == 0UL)\n"
                     "        stack[sp - 3] = stack[sp - 2];\n"
                     "    sp -= 2;\n";
            return;
        }
        if (code == 0x1a) {
            body_ << "    --sp;\n";
            return;
        }
        if (const auto* op = opcode_info(code); op && code != 0 && code != 1) {
            const bool is_unary = op->operation == Operation::eqz || op->operation == Operation::clz ||
                op->operation == Operation::ctz || op->operation == Operation::popcnt;
            depth_ = is_unary ? 1 : 2;
            body_ << "    {\n    struct wasm_word *s = stack + (sp - " << depth_ << ");\n";
            emit(*op, 0);
            if (!is_unary)
                body_ << "    --sp;\n";
            body_ << "    }\n";
            return;
        }
        if (const bool wide = code >= 0x51; (code >= 0x46 && code <= 0x4f) ||
                                           (code >= 0x51 && code <= 0x5a)) {
            const auto kind = code - (wide ? 0x51 : 0x46);
            constexpr std::string_view test[]{"==", "!=", "<", "<", ">", ">", "<=", "<=", ">=", ">="};
            body_ << "    --sp;\n"
                     "    stack[sp - 1].lo = wasm_cmp(stack[sp - 1], stack[sp], "
                  << (kind >= 2 && (kind % 2) == 0) << ", " << wide << ") " << test[kind] << " 0;\n"
                     "    stack[sp - 1].hi = 0UL;\n";
            return;
        }
        if (const bool wide = code >= 0x7f; (code >= 0x6d && code <= 0x70) ||
                                           (code >= 0x7f && code <= 0x82)) {
            const auto kind = code - (wide ? 0x7f : 0x6d);
            body_ << "    --sp;\n"
                     "    status = wasm_divide(&stack[sp - 1], stack[sp - 1], stack[sp], "
                  << ((kind % 2) == 0) << ", " << (kind >= 2) << ", " << wide << ");\n"
                     "    if (status)\n"
                     "        goto wasm_fail;\n";
            use(Helper::sub64, "wasm_sub64");
            use(Helper::shl64, "wasm_shl64");
            return;
        }
        if (code == 0xa7 || code == 0xad) {
            body_ << "    stack[sp - 1].hi = 0UL;\n";
            return;
        }
        if (code == 0xac || (code >= 0xc0 && code <= 0xc4)) {
            const auto bits = code == 0xc0 || code == 0xc2 ? 8U : code == 0xc1 || code == 0xc3 ? 16U : 32U;
            const auto sign = std::uint32_t{1} << (bits - 1);
            const auto mask = sign | (sign - 1);
            if (code == 0xac || code >= 0xc2)
                body_ << "    stack[sp - 1].hi = (stack[sp - 1].lo & " << sign
                      << "UL) ? 0xffffffffUL : 0UL;\n";
            if (bits < 32)
                body_ << "    stack[sp - 1].lo = ((stack[sp - 1].lo & " << mask << "UL) ^ "
                      << sign << "UL) - " << sign << "UL;\n"
                         "    stack[sp - 1].lo &= 0xffffffffUL;\n";
            return;
        }
        if (code >= 0x5b && code <= 0x66) {
            const bool wide = code >= 0x61;
            enable_fp();
            body_ << "    --sp;\n    wasm_fp_compare(&stack[sp - 1], stack[sp], "
                  << wide << ", " << (code - (wide ? 0x61 : 0x5b)) << "U);\n";
            return;
        }
        if (code >= 0x8b && code <= 0xa6) {
            const bool wide = code >= 0x99;
            const auto kind = code - (wide ? 0x99 : 0x8b);
            const auto* limb = wide ? ".hi" : ".lo";
            if (kind == 0 || kind == 1) {
                body_ << "    stack[sp - 1]" << limb
                      << (kind == 0 ? " &= 0x7fffffffUL;\n" : " ^= 0x80000000UL;\n");
            } else if (kind == 13) {
                body_ << "    --sp;\n    stack[sp - 1]" << limb << " = (stack[sp - 1]" << limb
                      << " & 0x7fffffffUL) | (stack[sp]" << limb << " & 0x80000000UL);\n";
            } else {
                enable_fp();
                if (kind >= 7)
                    body_ << "    --sp;\n";
                body_ << "    stack[sp - 1] = wasm_fp_arithmetic(stack[sp - 1], stack["
                      << (kind >= 7 ? "sp" : "sp - 1") << "], " << wide << ", " << kind << "U);\n";
            }
            return;
        }
        if ((code >= 0xa8 && code <= 0xab) || (code >= 0xae && code <= 0xb1)) {
            fp_trunc(code - (code >= 0xae ? 0xaa : 0xa8), false);
            return;
        }
        if (code >= 0xb2 && code <= 0xbb) {
            const bool wide = code >= 0xb7;
            enable_fp();
            if (code == 0xb6 || code == 0xbb) {
                body_ << "    stack[sp - 1] = wasm_fp_resize(stack[sp - 1], " << wide << ");\n";
            } else {
                const auto kind = code - (wide ? 0xb7 : 0xb2);
                body_ << "    stack[sp - 1] = wasm_integer_to_fp(stack[sp - 1], " << (kind >= 2)
                      << ", " << wide << ", " << ((kind & 1) == 0) << ");\n";
            }
            return;
        }
        if (code >= 0xbc && code <= 0xbf)
            return; // Reinterpretation is an identity on the raw limb representation.
    }
    if (code == 0xfc && insn.opcode.extension <= 7) {
        fp_trunc(insn.opcode.extension, true);
        return;
    }
    throw Error(insn.offset, "instruction not supported by the C90 module backend: " +
                             std::string(core::lookup(insn.opcode)->mnemonic));
}

struct ModuleScope {
    std::size_t start{}, alternative{}, params{}, results{};
    bool is_loop{}, branched{};
};

void c_bytes(std::ostream& out, const std::string& name, std::string_view bytes) {
    // Avoid translation limits for long export names and type lists, and
    // keep metadata independent of the execution character set.
    out << "static const unsigned char " << name << "[] = {";
    if (bytes.empty())
        out << '0';
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i % 16 == 0)
            out << "\n    ";
        out << static_cast<unsigned>(static_cast<unsigned char>(bytes[i])) << ", ";
    }
    out << "};\n";
}

std::string Emitter::run_module(const core::Module& module) {
    using core::ExternalKind;
    for (const auto& imp : module.imports)
        if (!std::holds_alternative<std::uint32_t>(imp.desc))
            throw Error(0, "non-function imports are not supported by the C90 module backend");
    constexpr std::size_t max_words = 32'767;
    constexpr std::size_t max_controls = 512;
    const auto imported = module.imports.size();
    const auto total_functions = imported + module.functions.size();
    if (total_functions > UINT32_MAX)
        throw Error(0, "too many functions for the C90 module backend");
    const auto type_index = [&](std::size_t index) {
        return index < imported ? std::get<std::uint32_t>(module.imports[index].desc)
                                : module.functions[index - imported].type;
    };
    const auto ftype = [&](std::size_t index) -> const core::FunctionType& {
        return module.types[type_index(index)];
    };
    for (const auto& type : module.types) {
        if (!std::ranges::all_of(type.params, is_word) || !std::ranges::all_of(type.results, is_word))
            throw Error(0, "SIMD value types are not supported by the C90 module backend");
        if (type.params.size() > max_words || type.results.size() > max_words)
            throw Error(0, "function type exceeds C90 module backend size limits");
    }
    for (const auto& g : module.globals)
        if (!is_word(g.type.type))
            throw Error(g.init.front().offset, "SIMD globals are not supported by the C90 module backend");

    bool compares = false;
    bool divides = false;
    bool data_init = false;
    bool table_init = false, table_grow = false, indirect_calls = false;
    bool memory_bounds = std::ranges::any_of(module.exports, [](const auto& e) {
        return e.kind == ExternalKind::memory;
    });
    std::vector<bool> data_bytes(module.data.size());
    for (std::size_t i = 0; i < module.data.size(); ++i)
        if (module.data[i].mode == core::SegmentMode::active)
            data_bytes[i] = data_init = true;
    std::vector<bool> element_values(module.elements.size());
    for (std::size_t i = 0; i < module.elements.size(); ++i)
        if (module.elements[i].mode == core::SegmentMode::active)
            element_values[i] = table_init = true;
    std::vector<std::size_t> signature_ids;
    const auto index_signatures = [&] {
        std::map<std::pair<std::vector<core::Type>, std::vector<core::Type>>, std::size_t> ids;
        for (const auto& type : module.types)
            signature_ids.push_back(ids.try_emplace({type.params, type.results}, ids.size()).first->second);
    };
    for (std::size_t f = imported; f < total_functions; ++f) {
        const auto& fn = module.functions[f - imported].body;
        const auto& type = ftype(f);
        const auto& instructions = fn.instructions;
        const auto none = instructions.size();
        auto locals = type.params.size();
        if (locals > max_words || instructions.size() > max_words || type.results.size() > max_words)
            throw Error(fn.offset, "function exceeds C90 module backend size limits");
        for (const auto& run : fn.locals) {
            if (!is_word(run.type))
                throw Error(fn.offset, "SIMD locals are not supported by the C90 module backend");
            if (run.count > max_words - locals)
                throw Error(fn.offset, "too many locals for the C90 module backend");
            locals += run.count;
        }

        // Encode control links once. A taken branch copies its label operands
        // down to the height saved on entry to the target and discards the rest.
        std::vector<ModuleScope> scopes{{none, none, 0, type.results.size(), false, false}};
        std::vector<std::size_t> nested{0}, owner(none);
        auto words = instructions.size();
        for (std::size_t i = 0; i < instructions.size(); ++i) {
            const auto& insn = instructions[i];
            const auto byte = insn.opcode.byte;
            if (byte >= 2 && byte <= 4) {
                const auto& block = std::get<core::BlockType>(insn.immediate).value;
                ModuleScope scope{i, none, 0, 0, byte == 3, false};
                if (const auto value = std::get_if<core::Type>(&block)) {
                    if (!is_word(*value))
                        throw Error(insn.offset, "SIMD block values are not supported by the C90 module backend");
                    scope.results = 1;
                } else if (const auto index = std::get_if<std::uint32_t>(&block)) {
                    scope.params = module.types[*index].params.size();
                    scope.results = module.types[*index].results.size();
                }
                if (nested.size() == max_controls)
                    throw Error(insn.offset, "control nesting exceeds C90 module backend limit");
                owner[i] = scopes.size();
                nested.push_back(scopes.size());
                scopes.push_back(scope);
            } else if (byte == 5 || byte == 0x0b) {
                owner[i] = nested.back();
                auto& scope = scopes[owner[i]];
                if (byte == 5)
                    scope.alternative = i;
                else
                    nested.pop_back();
            } else if (byte >= 0x0c && byte <= 0x0f) {
                const auto mark = [&](std::size_t index) {
                    scopes[index].branched = true;
                };
                if (byte == 0x0f) {
                    mark(0);
                } else if (byte == 0x0e) {
                    const auto& table = std::get<core::BranchTable>(insn.immediate);
                    for (auto label : table.labels)
                        mark(nested[nested.size() - 1 - label]);
                    mark(nested[nested.size() - 1 - table.otherwise]);
                } else {
                    const auto label = std::get<std::uint32_t>(insn.immediate);
                    mark(nested[nested.size() - 1 - label]);
                }
            } else if (byte == 0x10 || byte == 0x11) {
                const auto& call_type = byte == 0x10 ? ftype(std::get<std::uint32_t>(insn.immediate))
                    : module.types[std::get<core::IndexPair>(insn.immediate).first];
                const auto returns = call_type.results.size();
                if (returns > max_words - words)
                    throw Error(insn.offset, "operand stack exceeds C90 module backend size limit");
                words += returns;
            }
        }
        // Detect the rare case of a direct call to a zero-parameter function
        // returning more values than any instruction in the caller pushes.
        words = std::max(words, type.results.size());

        body_ << "\nstatic int wasm_f" << f << "(struct wasm_instance *instance, struct wasm_word *out,\n"
                 "                   const struct wasm_word *args, unsigned long *fuel, unsigned depth)\n{\n"
                 "    struct wasm_word *workspace, *stack;\n"
                 "    int status;\n"
                 "    size_t sp = 0;\n";
        for (std::size_t s = 1; s < scopes.size(); ++s)
            if (scopes[s].branched)
                body_ << "    size_t base_" << s << ";\n";
        // Only the outer frame is implicit; its base is always zero.
        body_ << "    (void)instance;\n    (void)args;\n    (void)out;\n    (void)sp;\n"
                 "    if (depth >= 128U)\n        return WASM_TRAP_RESOURCE;\n"
                 "    workspace = wasm_alloc(" << (locals + words) << "UL);\n"
                 "    if (workspace == NULL)\n        return WASM_TRAP_RESOURCE;\n"
                 "    stack = workspace + " << locals << "UL;\n    (void)stack;\n";
        if (!type.params.empty())
            body_ << "    memcpy(workspace, args, " << type.params.size() << "UL * sizeof(*args));\n";
        nested.push_back(0);
        const auto branch = [&](std::size_t s, std::string_view indent) {
            const auto& scope = scopes[s];
            const auto n = scope.is_loop ? scope.params : scope.results;
            const auto base = s == 0 ? std::string("0") : "base_" + std::to_string(s);
            if (n)
                body_ << indent << "memmove(stack + " << base << ", stack + (sp - " << n
                      << "), " << n << "UL * sizeof(*stack));\n";
            body_ << indent << "sp = " << base << " + " << n << ";\n"
                  << indent << "goto wasm_" << (scope.is_loop ? "loop_" : "end_") << s << ";\n";
        };
        for (std::size_t i = 0; i < instructions.size(); ++i) {
            const auto& insn = instructions[i];
            const auto byte = insn.opcode.byte;
            const auto s = owner[i];
            auto& scope = scopes[s];
            if (byte == 0x0b && scope.start != none && scope.alternative == none &&
                instructions[scope.start].opcode.byte == 4)
                body_ << "wasm_else_" << s << ":\n";
            body_ << "    if (*fuel == 0UL) {\n"
                     "        status = WASM_TRAP_FUEL;\n        goto wasm_fail;\n    }\n    --*fuel;\n";
            switch (byte) {
            case 0x00:
                body_ << "    status = WASM_TRAP_UNREACHABLE;\n    goto wasm_fail;\n";
                break;
            case 0x01: break;
            case 0x02: case 0x03: case 0x04:
                nested.push_back(s);
                if (byte == 0x04)
                    body_ << "    --sp;\n";
                if (scope.branched)
                    body_ << "    base_" << s << " = sp - " << scope.params << ";\n";
                if (byte == 0x03 && scope.branched)
                    body_ << "wasm_loop_" << s << ": ;\n";
                if (byte == 0x04)
                    body_ << "    if (stack[sp].lo == 0UL)\n        goto wasm_else_" << s << ";\n";
                break;
            case 0x05:
                body_ << "    goto wasm_end_" << s << ";\nwasm_else_" << s << ": ;\n";
                break;
            case 0x0b:
                nested.pop_back();
                if ((scope.branched && !scope.is_loop) || scope.alternative != none)
                    body_ << "wasm_end_" << s << ": ;\n";
                break;
            case 0x0c: case 0x0d: case 0x0f:
                if (byte == 0x0d)
                    body_ << "    if (stack[--sp].lo != 0UL) {\n";
                branch(byte == 0x0f ? 0 : nested[nested.size() - 1 -
                       std::get<std::uint32_t>(insn.immediate)], byte == 0x0d ? "        " : "    ");
                if (byte == 0x0d)
                    body_ << "    }\n";
                break;
            case 0x0e: {
                const auto& table = std::get<core::BranchTable>(insn.immediate);
                body_ << "    switch (stack[--sp].lo) {\n";
                for (std::size_t j = 0; j < table.labels.size(); ++j) {
                    body_ << "    case " << j << "UL:\n";
                    branch(nested[nested.size() - 1 - table.labels[j]], "        ");
                }
                body_ << "    default:\n";
                branch(nested[nested.size() - 1 - table.otherwise], "        ");
                body_ << "    }\n";
                break;
            }
            case 0x10: {
                const auto target = std::get<std::uint32_t>(insn.immediate);
                const auto& target_type = ftype(target);
                body_ << "    sp -= " << target_type.params.size() << ";\n"
                      << "    status = wasm_functions[" << target
                      << "UL](instance, stack + sp, stack + sp, fuel, depth + 1U);\n"
                         "    if (status)\n"
                         "        goto wasm_fail;\n"
                         "    sp += " << target_type.results.size() << ";\n";
                break;
            }
            case 0x11: {
                const auto imm = std::get<core::IndexPair>(insn.immediate);
                const auto& type_ = module.types[imm.first];
                if (!indirect_calls) {
                    index_signatures();
                    indirect_calls = true;
                }
                body_ << "    {\n"
                      << "        struct wasm_table *table = &instance->tables[" << imm.second << "UL];\n"
                         "        unsigned long sel = stack[--sp].lo;\n"
                         "        unsigned long index;\n"
                         "        if (sel >= table->size) {\n"
                         "            status = WASM_TRAP_TABLE_BOUNDS;\n"
                         "            goto wasm_fail;\n        }\n"
                         "        index = table->values[sel].lo;\n"
                         "        if (index == 0UL) {\n"
                         "            status = WASM_TRAP_NULL_FUNCTION;\n"
                         "            goto wasm_fail;\n        }\n"
                         "        --index;\n"
                         "        if (wasm_function_types[index] != " << signature_ids[imm.first] << "UL) {\n"
                         "            status = WASM_TRAP_INDIRECT_TYPE;\n"
                         "            goto wasm_fail;\n        }\n"
                         "        sp -= " << type_.params.size() << ";\n"
                         "        status = wasm_functions[index](instance, stack + sp, stack + sp, fuel, depth + 1U);\n"
                         "        if (status)\n            goto wasm_fail;\n"
                         "        sp += " << type_.results.size() << ";\n    }\n";
                break;
            }
            case 0x23: case 0x24: {
                const auto index = std::get<std::uint32_t>(insn.immediate);
                if (byte == 0x23)
                    body_ << "    stack[sp++] = instance->globals[" << index << "UL];\n";
                else
                    body_ << "    instance->globals[" << index << "UL] = stack[--sp];\n";
                break;
            }
            case 0x25: case 0x26: {
                const auto index = std::get<std::uint32_t>(insn.immediate);
                const bool get = byte == 0x25;
                body_ << "    {\n"
                      << "        struct wasm_table *table = &instance->tables[" << index << "UL];\n"
                      << "        unsigned long sel = stack[sp - " << (get ? 1 : 2) << "].lo;\n"
                         "        if (sel >= table->size) {\n"
                         "            status = WASM_TRAP_TABLE_BOUNDS;\n"
                         "            goto wasm_fail;\n        }\n";
                if (get)
                    body_ << "        stack[sp - 1] = table->values[sel];\n";
                else
                    body_ << "        table->values[sel] = stack[sp - 1];\n        sp -= 2;\n";
                body_ << "    }\n";
                break;
            }
            case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d:
            case 0x2e: case 0x2f: case 0x30: case 0x31: case 0x32: case 0x33:
            case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
            case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: {
                constexpr unsigned widths[]{4,8,4,8,1,1,2,2,1,1,2,2,4,4,4,8,4,8,1,2,1,2,4};
                const auto mem = std::get<core::MemoryArg>(insn.immediate);
                const bool store = byte >= 0x36;
                const bool sign = byte == 0x2c || byte == 0x2e || byte == 0x30 ||
                                  byte == 0x32 || byte == 0x34;
                memory_access_ = true;
                if (store)
                    body_ << "    sp -= 2;\n";
                body_ << "    status = wasm_memory_access(&instance->memory, &stack["
                      << (store ? "sp + 1], stack[sp]" : "sp - 1], stack[sp - 1]")
                      << ".lo, " << mem.offset << "UL, " << widths[byte - 0x28] << "U, " << store
                      << ", " << sign << ", " << (byte >= 0x30) << ");\n"
                         "    if (status)\n        goto wasm_fail;\n";
                break;
            }
            case 0x3f:
                body_ << "    stack[sp].lo = instance->memory.pages;\n"
                         "    stack[sp++].hi = 0UL;\n";
                break;
            case 0x40:
                memory_grow_ = true;
                body_ << "    stack[sp - 1].lo = wasm_memory_grow(&instance->memory, stack[sp - 1].lo, "
                      << module.memories.front().limits.max.value_or(65536) << "UL);\n";
                break;
            case 0xfc:
                if (insn.opcode.extension >= 12) {
                    const auto op = insn.opcode.extension;
                    if (op == 12) {
                        const auto imm = std::get<core::IndexPair>(insn.immediate);
                        element_values[imm.first] = table_init = true;
                        body_ << "    sp -= 3;\n"
                              << "    status = wasm_table_init(&instance->tables[" << imm.second
                              << "UL], wasm_element_" << imm.first << ", instance->element_size["
                              << imm.first << "UL], stack[sp].lo, stack[sp + 1].lo, stack[sp + 2].lo);\n"
                                 "    if (status)\n        goto wasm_fail;\n";
                    } else if (op == 14) {
                        const auto imm = std::get<core::IndexPair>(insn.immediate);
                        body_ << "    {\n"
                              << "        struct wasm_table *dst = &instance->tables[" << imm.first << "UL];\n"
                              << "        struct wasm_table *src = &instance->tables[" << imm.second << "UL];\n"
                                 "        unsigned long d, s, n;\n"
                                 "        sp -= 3;\n"
                                 "        d = stack[sp].lo; s = stack[sp + 1].lo; n = stack[sp + 2].lo;\n"
                                 "        if (d > dst->size || n > dst->size - d ||\n"
                                 "            s > src->size || n > src->size - s) {\n"
                                 "            status = WASM_TRAP_TABLE_BOUNDS;\n"
                                 "            goto wasm_fail;\n        }\n"
                                 "        if (n != 0UL)\n"
                                 "            memmove(dst->values + d, src->values + s, (size_t)n * sizeof(*dst->values));\n"
                                 "    }\n";
                    } else {
                        const auto index = std::get<std::uint32_t>(insn.immediate);
                        if (op == 13) {
                            body_ << "    instance->element_size[" << index << "UL] = 0;\n";
                        } else if (op == 15) {
                            table_grow = true;
                            body_ << "    --sp;\n    stack[sp - 1].lo = wasm_table_grow(&instance->tables["
                                  << index << "UL], stack[sp].lo, "
                                  << module.tables[index].limits.max.value_or(UINT32_MAX)
                                  << "UL, stack[sp - 1]);\n    stack[sp - 1].hi = 0UL;\n";
                        } else if (op == 16) {
                            body_ << "    stack[sp].lo = instance->tables[" << index << "UL].size;\n"
                                     "    stack[sp++].hi = 0UL;\n";
                        } else {
                            body_ << "    {\n"
                                  << "        struct wasm_table *table = &instance->tables[" << index << "UL];\n"
                                     "        unsigned long d, n, i;\n"
                                     "        sp -= 3;\n"
                                     "        d = stack[sp].lo; n = stack[sp + 2].lo;\n"
                                     "        if (d > table->size || n > table->size - d) {\n"
                                     "            status = WASM_TRAP_TABLE_BOUNDS;\n"
                                     "            goto wasm_fail;\n        }\n"
                                     "        for (i = 0UL; i < n; ++i)\n"
                                     "            table->values[d + i] = stack[sp + 1];\n"
                                     "    }\n";
                        }
                    }
                    break;
                }
                if (insn.opcode.extension == 8 || insn.opcode.extension == 9) {
                    const auto index = std::get<std::uint32_t>(insn.immediate);
                    if (insn.opcode.extension == 8) {
                        data_bytes[index] = data_init = true;
                        body_ << "    sp -= 3;\n"
                              << "    status = wasm_memory_init(&instance->memory, wasm_data_" << index
                              << ", instance->data_size[" << index
                              << "UL], stack[sp].lo, stack[sp + 1].lo, stack[sp + 2].lo);\n"
                                 "    if (status)\n        goto wasm_fail;\n";
                    } else {
                        body_ << "    instance->data_size[" << index << "UL] = 0;\n";
                    }
                    break;
                }
                if (insn.opcode.extension == 10 || insn.opcode.extension == 11) {
                    const bool copy = insn.opcode.extension == 10;
                    memory_bounds = true;
                    body_ << "    {\n"
                          << "        size_t dst" << (copy ? ", src" : "") << ", n;\n"
                             "        sp -= 3;\n"
                             "        n = (size_t)stack[sp + 2].lo;\n"
                             "        if (n != stack[sp + 2].lo ||\n"
                             "            wasm_memory_range(&instance->memory, stack[sp].lo, 0UL, n, &dst)";
                    if (copy)
                        body_ << " ||\n"
                                 "            wasm_memory_range(&instance->memory, stack[sp + 1].lo, 0UL, n, &src)";
                    body_ << ") {\n"
                             "            status = WASM_TRAP_MEMORY_BOUNDS;\n"
                             "            goto wasm_fail;\n"
                             "        }\n"
                             "        if (n != 0)\n";
                    if (copy)
                        body_ << "            memmove(instance->memory.bytes + dst, instance->memory.bytes + src, n);\n";
                    else
                        body_ << "            memset(instance->memory.bytes + dst, (int)(stack[sp + 1].lo & 0xffUL), n);\n";
                    body_ << "    }\n";
                    break;
                }
                module_opcode(insn);
                break;
            case 0x20: case 0x21: case 0x22: {
                const auto index = std::get<std::uint32_t>(insn.immediate);
                if (byte == 0x20)
                    body_ << "    stack[sp++] = workspace[" << index << "UL];\n";
                else
                    body_ << "    workspace[" << index << "UL] = stack["
                          << (byte == 0x21 ? "--sp" : "sp - 1") << "];\n";
                break;
            }
            default:
                compares = compares || (byte >= 0x46 && byte <= 0x4f) || (byte >= 0x51 && byte <= 0x5a);
                divides = divides || (byte >= 0x6d && byte <= 0x70) || (byte >= 0x7f && byte <= 0x82);
                module_opcode(insn);
            }
        }
        if (!type.results.empty())
            body_ << "    memcpy(out, stack, " << type.results.size() << "UL * sizeof(*out));\n";
        body_ << "    free(workspace);\n    return 0;\n"
                 "wasm_fail:\n    free(workspace);\n    return status;\n}\n";
    }

    auto source = std::string(module_preamble);
    source += word_type;
    source += module_support;
    source += module_alloc;
    if (imported)
        source += module_imports;
    if (!module.memories.empty())
        source += module_memory;
    if (memory_bounds || memory_access_ || data_init)
        source += module_memory_bounds;
    if (memory_access_)
        source += module_memory_access;
    if (memory_grow_)
        source += module_memory_grow;
    if (data_init)
        source += module_data_init;
    if (!module.tables.empty())
        source += module_table;
    if (table_init)
        source += module_table_init;
    if (table_grow)
        source += module_table_grow;
    for (const auto& [helper, code] : helpers)
        if (used_[static_cast<std::size_t>(helper)])
            source += code;
    if (compares || divides)
        source += module_compare;
    if (divides)
        source += module_division;
    if (fp_)
        source += detail::module_float_support;
    std::ostringstream top;
    top.imbue(std::locale::classic());
    top << "\nstruct wasm_instance {\n    unsigned running;\n";
    if (!module.memories.empty())
        top << "    struct wasm_memory memory;\n";
    if (!module.globals.empty())
        top << "    struct wasm_word globals[" << module.globals.size() << "UL];\n";
    if (!module.data.empty())
        top << "    size_t data_size[" << module.data.size() << "UL];\n";
    if (!module.tables.empty())
        top << "    struct wasm_table tables[" << module.tables.size() << "UL];\n";
    if (!module.elements.empty())
        top << "    unsigned long element_size[" << module.elements.size() << "UL];\n";
    if (imported)
        top << "    struct wasm_binding bindings[" << imported << "UL];\n";
    top << "};\n";
    if (indirect_calls) {
        top << "static const unsigned long wasm_function_types[] = {\n";
        for (std::size_t f = 0; f < total_functions; ++f)
            top << "    " << signature_ids[type_index(f)] << "UL,\n";
        top << "    0UL\n};\n";
    }
    for (std::size_t i = 0; i < module.elements.size(); ++i) {
        if (!element_values[i])
            continue;
        top << "static const struct wasm_word wasm_element_" << i << "[] = {\n";
        if (module.elements[i].init.empty())
            top << "    {0UL, 0UL},\n";
        for (const auto& expr : module.elements[i].init) {
            const auto bits = constant_bits(expr.front());
            top << "    {" << static_cast<std::uint32_t>(bits) << "UL, "
                << static_cast<std::uint32_t>(bits >> 32) << "UL},\n";
        }
        top << "};\n";
    }
    for (std::size_t i = 0; i < module.data.size(); ++i) {
        const auto& bytes = module.data[i].bytes;
        if (!data_bytes[i])
            continue;
        top << "static const unsigned char wasm_data_" << i << "[] = {";
        if (bytes.empty())
            top << "0";
        for (std::size_t j = 0; j < bytes.size(); ++j) {
            if (j % 16 == 0)
                top << "\n    ";
            top << static_cast<unsigned>(bytes[j]) << ", ";
        }
        top << "};\n";
    }
    const auto tags = [](const auto& types) {
        std::string result;
        for (const auto t : types)
            result += static_cast<char>(t);
        return result;
    };
    if (imported) {
        for (std::size_t i = 0; i < imported; ++i) {
            const auto& imp = module.imports[i];
            const auto& type = ftype(i);
            if (imp.module.size() > max_words || imp.name.size() > max_words)
                throw Error(0, "import name exceeds C90 module backend size limit");
            const auto label = "wasm_i" + std::to_string(i);
            c_bytes(top, label + "_module", imp.module);
            c_bytes(top, label + "_name", imp.name);
            c_bytes(top, label + "_params", tags(type.params));
            c_bytes(top, label + "_results", tags(type.results));
        }
        top << "static const struct wasm_import_desc wasm_import_descs[] = {\n";
        for (std::size_t i = 0; i < imported; ++i) {
            const auto& imp = module.imports[i];
            const auto& type = ftype(i);
            const auto label = "wasm_i" + std::to_string(i);
            top << "    {" << label << "_module, " << label << "_name, " << label << "_params, "
                << label << "_results, " << imp.module.size() << "UL, " << imp.name.size() << "UL, "
                << type.params.size()
                << "UL, " << type.results.size() << "UL},\n";
        }
        top << "};\n";
        for (std::size_t i = 0; i < imported; ++i) {
            top << "static int wasm_f" << i << "(struct wasm_instance *instance, struct wasm_word *out,\n"
                   "                   const struct wasm_word *args, unsigned long *fuel, unsigned depth)\n"
                   "{\n"
                   "    return wasm_call_import(&instance->bindings[" << i << "UL], &wasm_import_descs["
                << i << "UL], out, args, fuel, depth);\n}\n";
        }
    }
    for (std::size_t f = imported; f < total_functions; ++f)
        top << "static int wasm_f" << f
            << "(struct wasm_instance *, struct wasm_word *, const struct wasm_word *, unsigned long *, unsigned);\n";
    top << "static const wasm_func wasm_functions[] = {\n";
    for (std::size_t f = 0; f < total_functions; ++f)
        top << "    wasm_f" << f << ",\n";
    top << "    NULL\n};\n"
        << "#define WASM_FUNCTION_COUNT " << total_functions << "UL\n";
    for (std::size_t i = 0; i < module.exports.size(); ++i) {
        const auto& e = module.exports[i];
        if (e.name.size() > max_words)
            throw Error(0, "export name exceeds C90 module backend size limit");
        const auto label = "wasm_e" + std::to_string(i);
        c_bytes(top, label + "_name", e.name);
        if (e.kind == ExternalKind::function) {
            const auto& type = ftype(e.index);
            c_bytes(top, label + "_params", tags(type.params));
            c_bytes(top, label + "_results", tags(type.results));
        }
    }
    top << "struct wasm_export {\n"
           "    const unsigned char *name, *params, *results;\n"
           "    size_t name_size, nargs, nresults;\n"
           "    unsigned long index;\n"
           "    unsigned kind, type, mutable_global;\n"
           "};\n"
           "static const struct wasm_export wasm_exports[] = {\n";
    for (std::size_t i = 0; i < module.exports.size(); ++i) {
        const auto& entry_ = module.exports[i];
        const auto label = "wasm_e" + std::to_string(i);
        top << "    {" << label << "_name, ";
        if (entry_.kind == ExternalKind::function) {
            const auto& type = ftype(entry_.index);
            top << label << "_params, " << label << "_results, "
                << entry_.name.size() << "UL, " << type.params.size() << "UL, "
                << type.results.size() << "UL, " << entry_.index << "UL, 0U, 0U, 0U},\n";
        } else {
            top << "NULL, NULL, " << entry_.name.size() << "UL, 0UL, 0UL, " << entry_.index << "UL, "
                << static_cast<unsigned>(entry_.kind) << "U, ";
            if (entry_.kind == ExternalKind::global) {
                const auto& type = module.globals[entry_.index].type;
                top << static_cast<unsigned>(type.type) << "U, " << type.mutable_ << "U},\n";
            } else if (entry_.kind == ExternalKind::table) {
                top << static_cast<unsigned>(module.tables[entry_.index].element) << "U, 0U},\n";
            } else {
                top << "0U, 0U},\n";
            }
        }
    }
    top << "    {NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0}\n};\n";
    source += top.str();
    source += body_.str();

    std::ostringstream api;
    api.imbue(std::locale::classic());
    api << R"C(
void wasm_instance_destroy(struct wasm_instance *instance)
{
)C";
    if (!module.tables.empty())
        api << "    size_t i;\n";
    api << R"C(
    if (instance == NULL || instance->running)
        return;
)C";
    if (!module.memories.empty())
        api << "    free(instance->memory.bytes);\n";
    if (!module.tables.empty())
        api << "    for (i = 0; i < " << module.tables.size() << "UL; ++i)\n"
               "        free(instance->tables[i].values);\n";
    api << R"C(    free(instance);
}

int wasm_instance_create_with_imports(struct wasm_instance **out,
                                      const struct wasm_host_import *imports, size_t nimports,
                                      unsigned long start_fuel)
{
    struct wasm_instance *instance;
    int status = WASM_OK;
)C";
    if (imported)
        api << "    size_t i, j;\n";
    api << R"C(
    (void)start_fuel;
    if (out == NULL)
        return WASM_INVALID_ARGUMENT;
    *out = NULL;
)C";
    if (imported) {
        api << R"C(    if (nimports != 0 && imports == NULL)
        return WASM_INVALID_ARGUMENT;
    for (i = 0; i < nimports; ++i) {
        if (imports[i].module == NULL || imports[i].name == NULL || imports[i].callback == NULL)
            return WASM_INVALID_ARGUMENT;
        for (j = 0; j < i; ++j)
            if (wasm_same_name(imports[i].module, imports[i].module_size, imports[j].module, imports[j].module_size) &&
                wasm_same_name(imports[i].name, imports[i].name_size, imports[j].name, imports[j].name_size))
                return WASM_INVALID_ARGUMENT;
    }
)C";
    } else {
        api << "    (void)imports;\n    if (nimports != 0)\n        return WASM_INVALID_ARGUMENT;\n";
    }
    if (fp_)
        api << "    if (!wasm_fp_supported())\n        return WASM_UNSUPPORTED_HOST;\n";
    api << R"C(
    instance = (struct wasm_instance *)calloc(1, sizeof(*instance));
    if (instance == NULL)
        return WASM_TRAP_RESOURCE;
)C";
    for (std::size_t i = 0; i < module.tables.size(); ++i)
        api << "    instance->tables[" << i << "UL].values = NULL;\n";
    if (!module.memories.empty())
        api << "    instance->memory.bytes = NULL;\n";
    if (imported) {
        api << "    for (i = 0; i < " << imported << "UL; ++i) {\n" << R"C(        const struct wasm_import_desc *desc = &wasm_import_descs[i];
        for (j = 0; j < nimports; ++j)
            if (wasm_same_name(desc->module, desc->module_size, imports[j].module, imports[j].module_size) &&
                wasm_same_name(desc->name, desc->name_size, imports[j].name, imports[j].name_size))
                break;
        if (j == nimports) {
            status = WASM_MISSING_IMPORT;
            goto wasm_create_fail;
        }
        instance->bindings[i].callback = imports[j].callback;
        instance->bindings[i].context = imports[j].context;
    }
)C";
    }
    if (!module.memories.empty()) {
        api << "    instance->memory.pages = " << module.memories.front().limits.min << "UL;\n"
               "    if (instance->memory.pages > (size_t)-1 / 65536UL) {\n"
               "        status = WASM_TRAP_RESOURCE;\n        goto wasm_create_fail;\n    }\n"
               "    instance->memory.size = (size_t)instance->memory.pages * 65536UL;\n"
               "    if (instance->memory.size != 0) {\n"
               "        instance->memory.bytes = (unsigned char *)calloc(instance->memory.size, 1);\n"
               "        if (instance->memory.bytes == NULL) {\n"
               "            status = WASM_TRAP_RESOURCE;\n            goto wasm_create_fail;\n        }\n    }\n";
    }
    for (std::size_t i = 0; i < module.globals.size(); ++i)
        emit_bits(api, "instance->globals[" + std::to_string(i) + "UL]",
                  constant_bits(module.globals[i].init.front()));
    for (std::size_t i = 0; i < module.tables.size(); ++i) {
        const auto size = module.tables[i].limits.min;
        if (size != 0) {
            const auto table = "instance->tables[" + std::to_string(i) + "UL]";
            api << "    " << table << ".size = " << size << "UL;\n"
                << "    " << table << ".values = wasm_alloc(" << size << "UL);\n"
                << "    if (" << table << ".values == NULL) {\n"
                   "        status = WASM_TRAP_RESOURCE;\n        goto wasm_create_fail;\n    }\n";
        }
    }
    for (std::size_t i = 0; i < module.elements.size(); ++i) {
        const auto& ele = module.elements[i];
        if (ele.mode == core::SegmentMode::active) {
            api << "    status = wasm_table_init(&instance->tables[" << ele.table << "UL], wasm_element_"
                << i << ", " << ele.init.size() << "UL, " << constant_bits(ele.offset.front())
                << "UL, 0UL, " << ele.init.size() << "UL);\n"
                   "    if (status)\n        goto wasm_create_fail;\n";
        } else if (ele.mode == core::SegmentMode::passive && element_values[i]) {
            api << "    instance->element_size[" << i << "UL] = " << ele.init.size() << "UL;\n";
        }
    }
    for (std::size_t i = 0; i < module.data.size(); ++i) {
        const auto& data = module.data[i];
        if (data.mode == core::SegmentMode::active) {
            api << "    status = wasm_memory_init(&instance->memory, wasm_data_" << i << ", "
                << data.bytes.size() << "UL, " << constant_bits(data.offset.front())
                << "UL, 0UL, " << data.bytes.size() << "UL);\n"
                   "    if (status)\n        goto wasm_create_fail;\n";
        } else if (data_bytes[i]) {
            api << "    instance->data_size[" << i << "UL] = sizeof(wasm_data_" << i << ")"
                << (data.bytes.empty() ? " - 1U" : "") << ";\n";
        }
    }
    if (module.start)
        api << "    instance->running = 1;\n"
               "    status = wasm_functions[" << *module.start
            << "UL](instance, NULL, NULL, &start_fuel, 0);\n"
               "    instance->running = 0;\n"
               "    if (status)\n        goto wasm_create_fail;\n";
    api << "    *out = instance;\n    return status;\n";
    if (!module.memories.empty() || module.start || imported ||
        std::ranges::any_of(module.tables, [](const auto& t) { return t.limits.min != 0; }) ||
        std::ranges::any_of(module.elements, [](const auto& e) { return e.mode == core::SegmentMode::active; }))
        api << "wasm_create_fail:\n    wasm_instance_destroy(instance);\n    return status;\n";
    api << R"C(}

int wasm_instance_create(struct wasm_instance **out, unsigned long start_fuel)
{
    return wasm_instance_create_with_imports(out, NULL, 0, start_fuel);
}

static const struct wasm_export *wasm_find_export(const char *name, size_t name_size,
                                                  unsigned kind)
{
    size_t i;
    if (name == NULL)
        return NULL;
    for (i = 0; wasm_exports[i].name != NULL; ++i)
        if (kind == wasm_exports[i].kind && name_size == wasm_exports[i].name_size &&
            memcmp(name, wasm_exports[i].name, name_size) == 0)
            return &wasm_exports[i];
    return NULL;
}

static unsigned wasm_width(unsigned type)
{
    return type == 0x7fU || type == 0x7dU ? 32U : 64U;
}

static int wasm_valid_word(const struct wasm_result *v, unsigned type)
{
    unsigned width = wasm_width(type);
    return v->width == width && ((v->lo | v->hi) & ~0xffffffffUL) == 0UL &&
           (width != 32U || v->hi == 0UL) &&
           (type != 0x70U || (v->hi == 0UL && v->lo <= WASM_FUNCTION_COUNT));
}

int wasm_instance_invoke(struct wasm_instance *instance,
                         const char *name, size_t name_size,
                         const struct wasm_result *args, size_t nargs,
                         struct wasm_result *out, size_t capacity, size_t *count,
                         unsigned long fuel)
{
    const struct wasm_export *e = wasm_find_export(name, name_size, 0U);
    size_t i;
    struct wasm_word *slots;
    int status;
    if (count != NULL)
        *count = 0;
    if (instance == NULL || instance->running)
        return WASM_INVALID_ARGUMENT;
    if (e == NULL || e->nargs != nargs || (nargs != 0 && args == NULL))
        return WASM_INVALID_ARGUMENT;
    for (i = 0; i < nargs; ++i)
        if (!wasm_valid_word(&args[i], (unsigned)e->params[i]))
            return WASM_INVALID_ARGUMENT;
    slots = wasm_alloc(e->nargs > e->nresults ? e->nargs : e->nresults);
    if (slots == NULL)
        return WASM_TRAP_RESOURCE;
    for (i = 0; i < nargs; ++i) {
        slots[i].lo = args[i].lo;
        slots[i].hi = args[i].hi;
    }
    instance->running = 1;
    status = wasm_functions[e->index](instance, slots, slots, &fuel, 0);
    instance->running = 0;
    if (status) {
        free(slots);
        return status;
    }
    if (count != NULL)
        *count = e->nresults;
    if (e->nresults != 0 && (out == NULL || capacity < e->nresults)) {
        free(slots);
        return WASM_OUTPUT_TOO_SMALL;
    }
    for (i = 0; i < e->nresults; ++i) {
        out[i].width = wasm_width((unsigned)e->results[i]);
        out[i].lo = slots[i].lo;
        out[i].hi = slots[i].hi;
    }
    free(slots);
    return WASM_OK;
}
)C";
    const auto host_memory = [&](bool write) {
        const auto* buffer = write ? "data" : "out";
        api << "\nint wasm_instance_" << (write ? "write" : "read") << "_memory("
               "struct wasm_instance *instance,\n"
               "                              const char *name, size_t name_size,\n"
               "                              unsigned long offset, " << (write ? "const " : "")
            << "unsigned char *" << buffer << ", size_t size)\n{\n";
        if (std::ranges::any_of(module.exports, [](const auto& e) { return e.kind == ExternalKind::memory; })) {
            api << "    size_t pos;\n"
                   "    int status;\n"
                   "    if (instance == NULL || wasm_find_export(name, name_size, 2U) == NULL ||\n"
                   "        (size != 0 && " << buffer << " == NULL))\n        return WASM_INVALID_ARGUMENT;\n"
                   "    status = wasm_memory_range(&instance->memory, offset, 0UL, size, &pos);\n"
                   "    if (status)\n        return status;\n"
                   "    if (size != 0)\n        memcpy("
                << (write ? "instance->memory.bytes + pos, data" : "out, instance->memory.bytes + pos")
                << ", size);\n    return WASM_OK;\n";
        } else {
            api << "    (void)instance; (void)name; (void)name_size; (void)offset;\n"
                   "    (void)" << buffer << "; (void)size;\n    return WASM_INVALID_ARGUMENT;\n";
        }
        api << "}\n";
    };
    host_memory(false);
    host_memory(true);
    const auto host_global = [&](bool write) {
        const auto* buffer = write ? "value" : "out";
        api << "\nint wasm_instance_" << (write ? "set" : "get") << "_global("
               "struct wasm_instance *instance,\n"
               "                             const char *name, size_t name_size, "
            << (write ? "const " : "") << "struct wasm_result *" << buffer << ")\n{\n";
        if (!module.globals.empty()) {
            api << "    const struct wasm_export *e = wasm_find_export(name, name_size, 3U);\n"
                   "    if (instance == NULL || e == NULL || " << buffer << " == NULL";
            if (write)
                api << " || !e->mutable_global || !wasm_valid_word(value, e->type)";
            api << ")\n        return WASM_INVALID_ARGUMENT;\n";
            if (write)
                api << "    instance->globals[e->index].lo = value->lo;\n"
                       "    instance->globals[e->index].hi = value->hi;\n";
            else
                api << "    out->width = wasm_width(e->type);\n"
                       "    out->lo = instance->globals[e->index].lo;\n"
                       "    out->hi = instance->globals[e->index].hi;\n";
            api << "    return WASM_OK;\n";
        } else {
            api << "    (void)instance; (void)name; (void)name_size; (void)" << buffer
                << ";\n    return WASM_INVALID_ARGUMENT;\n";
        }
        api << "}\n";
    };
    host_global(false);
    host_global(true);
    const auto host_table = [&](bool write) {
        const auto* buffer = write ? "value" : "out";
        api << "\nint wasm_instance_" << (write ? "set" : "get") << "_table("
               "struct wasm_instance *instance, const char *name, size_t name_size,\n"
               "                            unsigned long index, " << (write ? "const " : "")
            << "struct wasm_result *" << buffer << ")\n{\n";
        if (!module.tables.empty()) {
            api << "    const struct wasm_export *e = wasm_find_export(name, name_size, 1U);\n"
                   "    struct wasm_table *table;\n"
                   "    if (instance == NULL || e == NULL || " << buffer << " == NULL";
            if (write)
                api << " || !wasm_valid_word(value, e->type)";
            api << ")\n        return WASM_INVALID_ARGUMENT;\n"
                   "    table = &instance->tables[e->index];\n"
                   "    if (index >= table->size)\n        return WASM_TRAP_TABLE_BOUNDS;\n";
            if (write)
                api << "    table->values[index].lo = value->lo;\n"
                       "    table->values[index].hi = value->hi;\n";
            else
                api << "    out->width = wasm_width(e->type);\n"
                       "    out->lo = table->values[index].lo;\n"
                       "    out->hi = table->values[index].hi;\n";
            api << "    return WASM_OK;\n";
        } else {
            api << "    (void)instance; (void)name; (void)name_size; (void)index;\n"
                   "    (void)" << buffer << ";\n    return WASM_INVALID_ARGUMENT;\n";
        }
        api << "}\n";
    };
    host_table(false);
    host_table(true);
    api << "\nint wasm_module_invoke" << module_signature << R"C(    struct wasm_instance *instance;
    int status;
    if (count != NULL)
        *count = 0;
    status = wasm_instance_create(&instance, 1000000UL);
    if (status)
        return 1;
    status = wasm_instance_invoke(instance, name, name_size, args, nargs, out, capacity, count, fuel);
    wasm_instance_destroy(instance);
    return status > WASM_INVALID_ARGUMENT ? 1 : status;
}
)C";
    source += api.str();
    return source;
}

} // namespace

std::string emit_c(std::span<const Instruction> program) {
    const auto validation = validate(program);
    return Emitter{}.run(program, validation);
}

std::string core::emit_module_c(const Module& module) {
    validate_module(module);
    return Emitter{}.run_module(module);
}

} // namespace limestone::wasmutils
