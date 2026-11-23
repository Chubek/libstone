/* Libstone software binary floating point. Finite operands are normalized
 * to a common 113-bit significand. Keeping three extra bits (guard, round,
 * sticky) until final packing gives subnormals and normals the same rounding
 * path. No floating expressions, wide division, or external math functions. */
#include "real_bits.h"

typedef __uint128_t U;
#define BIT(i) ((U)1 << (i))
enum { ZERO, FINITE, INFINITY_, NAN_ };
typedef struct { U sig; int exp, neg, kind; } Number;
typedef struct { unsigned frac, field; int bias; } Format;

static Format format(unsigned width)
{
    switch (width) {
    case 32: return (Format){23,23,127};
    case 64: return (Format){52,52,1023};
    case 80: return (Format){63,64,16383};
    default: return (Format){112,112,16383};
    }
}

static cephyr_real_bits words(U x)
{
    return (cephyr_real_bits){(uint64_t)x, (uint64_t)(x >> 64)};
}

static U jam(U n, unsigned shift)
{
    if (!shift) return n;
    if (shift >= 128) return n != 0;
    return (n >> shift) | ((n & (BIT(shift)-1)) != 0);
}

static Number unpack(unsigned width, cephyr_real_bits x)
{
    Format f = format(width);
    U raw = ((U)x.hi << 64) | x.lo;
    unsigned e = (unsigned)(raw >> f.field) & (unsigned)(2*f.bias+1);
    U frac = raw & (BIT(f.frac)-1);
    Number n = {0, 0, (int)((raw >> (width-1)) & 1), ZERO};
    if (e == (unsigned)(2*f.bias+1)) {
        n.kind = frac ? NAN_ : INFINITY_;
    } else if (e || frac || (width == 80 && (raw & BIT(63)))) {
        n.kind = FINITE;
        n.exp = (e ? (int)e : 1) - f.bias;
        n.sig = (frac | ((e || (width == 80 && (raw & BIT(63)))) ? BIT(f.frac) : 0))
                << (112-f.frac);
        while (n.sig < BIT(112)) { n.sig <<= 1; --n.exp; }
    }
    return n;
}

static cephyr_real_bits special(unsigned w, int neg, int kind)
{
    Format f = format(w);
    U raw = (U)neg << (w-1);
    if (kind != ZERO) {
        raw |= (U)(2*f.bias+1) << f.field;
        if (w == 80) raw |= BIT(63);
        if (kind == NAN_) raw |= BIT(f.frac-1);
    }
    return words(raw);
}

/* Input magnitude represents ext * 2^(exp-115). */
static cephyr_real_bits rounded(unsigned w, int neg, int exp, U ext)
{
    Format f = format(w);
    if (!ext) return special(w, neg, ZERO);
    while (ext >= BIT(116)) { ext = jam(ext,1); ++exp; }
    while (ext < BIT(115)) { ext <<= 1; --exp; }
    unsigned shift = 112 - f.frac;
    if (exp < 1-f.bias) { shift += (unsigned)(1-f.bias-exp); exp = 1-f.bias; }
    ext = jam(ext, shift);
    U sig = (ext >> 3) + ((ext & 7) > 4 || ((ext & 7) == 4 && (ext & 8)));
    if (sig >= BIT(f.frac+1)) { sig >>= 1; ++exp; }
    if (exp > f.bias) return special(w, neg, INFINITY_);
    U e = sig >= BIT(f.frac) ? (unsigned)(exp+f.bias) : 0;
    return words(((U)neg << (w-1)) | (e << f.field) | (sig & (BIT(f.field)-1)));
}

cephyr_real_bits cephyr_real_cast(unsigned to, unsigned from, cephyr_real_bits value)
{
    Number a = unpack(from,value);
    return a.kind == FINITE ? rounded(to,a.neg,a.exp,a.sig<<3) : special(to,a.neg,a.kind);
}

static int magnitude_cmp(Number a, Number b)
{
    if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
    if (a.kind != FINITE) return 0;
    if (a.exp != b.exp) return a.exp < b.exp ? -1 : 1;
    return (a.sig > b.sig) - (a.sig < b.sig);
}

int cephyr_real_cmp(unsigned w, cephyr_real_bits x, cephyr_real_bits y)
{
    Number a = unpack(w,x), b = unpack(w,y);
    if (a.kind == NAN_ || b.kind == NAN_) return 2;
    if (a.kind == ZERO && b.kind == ZERO) return 0;
    if (a.neg != b.neg) return a.neg ? -1 : 1;
    return a.neg ? -magnitude_cmp(a,b) : magnitude_cmp(a,b);
}

cephyr_real_bits cephyr_real_calc(unsigned w, char op, cephyr_real_bits x, cephyr_real_bits y)
{
    Number a = unpack(w,x), b = unpack(w,y);
    int neg = a.neg ^ b.neg;
    if (a.kind == NAN_ || b.kind == NAN_) return special(w,0,NAN_);
    if (op == '+' || op == '-') {
        b.neg ^= op == '-';
        if (a.kind == INFINITY_ && b.kind == INFINITY_ && a.neg != b.neg)
            return special(w,0,NAN_);
        if (magnitude_cmp(a,b) < 0) { Number t=a; a=b; b=t; }
        if (a.kind == ZERO) return special(w, a.neg && b.neg, ZERO);
        if (a.kind == INFINITY_) return special(w,a.neg,INFINITY_);
        U s = a.sig << 3;
        U t = b.kind == ZERO ? 0 : jam(b.sig << 3,(unsigned)(a.exp-b.exp));
        if (a.neg != b.neg && s == t) return special(w,0,ZERO);
        return rounded(w,a.neg,a.exp,a.neg == b.neg ? s+t : s-t);
    }
    if (op == '*') {
        if ((a.kind == ZERO && b.kind == INFINITY_) || (b.kind == ZERO && a.kind == INFINITY_))
            return special(w,0,NAN_);
        if (a.kind == INFINITY_ || b.kind == INFINITY_) return special(w,neg,INFINITY_);
        if (a.kind == ZERO || b.kind == ZERO) return special(w,neg,ZERO);
        /* Process multiplier bits least significant first. After each
         * addition the low bit is final: retain its contribution to sticky. */
        U prod = 0, multiplier = b.sig;
        unsigned sticky = 0;
        for (unsigned i=0; i<=112; ++i) {
            if (multiplier & 1) prod += a.sig << 3;
            multiplier >>= 1;
            if (i != 112) { sticky |= (unsigned)(prod & 1); prod >>= 1; }
        }
        return rounded(w,neg,a.exp+b.exp,prod | sticky);
    }
    if ((a.kind == ZERO && b.kind == ZERO) || (a.kind == INFINITY_ && b.kind == INFINITY_))
        return special(w,0,NAN_);
    if (a.kind == INFINITY_ || b.kind == ZERO) return special(w,neg,INFINITY_);
    if (a.kind == ZERO || b.kind == INFINITY_) return special(w,neg,ZERO);
    /* Normalize the quotient before generating its round bits; the exact
     * remainder after the last quotient digit supplies the sticky bit. */
    U rem = a.sig, q = 0;
    int exp = a.exp-b.exp;
    if (rem < b.sig) { rem <<= 1; --exp; }
    for (unsigned i=0; i<116; ++i) {
        q <<= 1;
        if (rem >= b.sig) { rem -= b.sig; ++q; }
        rem <<= 1;
    }
    return rounded(w,neg,exp,q | (rem != 0));
}

cephyr_real_bits cephyr_real_from_int(unsigned w, int neg, uint64_t magnitude)
{
    return rounded(w,neg,115,(U)magnitude);
}

uint64_t cephyr_real_to_int(unsigned w, cephyr_real_bits x, unsigned bits, int is_signed)
{
    Number a = unpack(w,x);
    U limit = BIT(bits - !!is_signed);
    if (a.kind == ZERO || (a.kind == FINITE && a.exp < 0) || (a.neg && !is_signed)) return 0;
    U mag = a.kind == FINITE && a.exp < 64 ? a.sig >> (112-a.exp) : limit;
    if (mag >= limit) mag = limit - !(a.neg && is_signed);
    return a.neg ? 0-(uint64_t)mag : (uint64_t)mag;
}
