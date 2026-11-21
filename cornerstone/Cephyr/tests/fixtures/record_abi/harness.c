#include "shared.h"

/* Freestanding Clang may lower a record copy to a libc call. */
void *memcpy(void *dest, const void *src, __SIZE_TYPE__ count) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    for (__SIZE_TYPE__ i = 0; i < count; ++i)
        d[i] = s[i];
    return dest;
}

struct Three clang_three(struct Three x, unsigned int y) {
    x.b[1] += y;
    x.b[2] ^= 255;
    return x;
}

struct Padded clang_padded(struct Padded x) {
    --x.tag;
    x.n += 0x10000000u;
    return x;
}

union Word clang_union(union Word x) {
    x.n ^= 0x1122334455667788ul;
    return x;
}

struct Pointer clang_pointer(struct Pointer x) {
    --*x.p;
    return x;
}

struct Nested clang_nested(const struct Nested x) {
    struct Nested r = {{{x.a[0].n ^ 0xffffu}, {x.a[1].n}}};
    return r;
}

struct Three clang_stack(unsigned long a, unsigned long b, unsigned long c, unsigned long d,
                         unsigned long e, unsigned long f, unsigned long g, unsigned long h,
                         struct Three x, unsigned long y, struct Padded z) {
    x.b[0] += a + b + c + d + e + f + g + h;
    x.b[1] += y;
    x.b[2] += z.n + z.tag;
    return x;
}

/* The volatile load prevents Clang from converting this into a direct call. */
static ThreeFn *volatile target = cephyr_three;
static NineFn *volatile wide_target = cephyr_nine;
static BigFn *volatile big_target = cephyr_big;

struct Nine clang_nine(struct Nine x, unsigned long y) {
    x.b[0] -= y;
    x.b[8] = x.b[7] + 1;
    return x;
}

struct TwoWords clang_seven(SEVEN) {
    struct TwoWords x = {a + b + 5*c + d, e + 11*f + g};
    return x;
}

unsigned long clang_split_scalar(SEVEN, struct TwoWords x, unsigned long tail) {
    return x.lo + x.hi + tail + a + b + c + d + e + f + g;
}

struct TwoWords clang_split(SEVEN, struct TwoWords x, unsigned long tail) {
    x.lo += a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g;
    x.hi -= tail;
    return x;
}

struct Nine clang_stacked(SEVEN, unsigned long h, struct Nine x, unsigned long tail) {
    x.b[0] -= a + b + c + d + e + f + g + h;
    x.b[8] -= tail;
    return x;
}

struct Big clang_big(unsigned long a, unsigned long b, unsigned long c, unsigned long d,
                     unsigned long e, unsigned long f, struct Big x, unsigned long tail) {
    // Volatile access forces Clang to overwrite the ABI's by-reference copy.
    // Cephyr must not pass the original C object into this call.
    volatile unsigned long *field = &x.a;
    *field -= a + 2*b + 3*c + 4*d + 5*e + 6*f;
    x.b ^= tail;
    x.c += *field;
    return x;
}

struct Seventeen clang_noargs(void) {
    struct Seventeen x = {{[0] = 2, [8] = 5, [16] = 6}};
    return x;
}

static int wide_test(void) {
    struct Nine x = {{213, 2, 3, 4, 5, 6, 7, 8, 242}};
    struct Nine y = cephyr_nine(x, 18);
    if (y.b[0] != 231 || y.b[7] != 8 || y.b[8] != 250)
        return 21;
    if (wide_target(x, 19).b[0] != 232 || x.b[8] != 242)
        return 22;
    struct TwoWords w = {0x123456789abcdef0ul, 0xf000000000000050ul};
    struct TwoWords z = cephyr_split(1, 2, 3, 4, 5, 6, 7, w, 32);
    if (z.lo != 0x123456789abcdf7cul || z.hi != 0xf000000000000070ul)
        return 23;
    y = cephyr_stacked(1, 2, 3, 4, 5, 6, 7, 8, x, 7);
    if (y.b[0] != 249 || y.b[8] != 249)
        return 24;
    struct Big b = {300, 0xa3, 400};
    struct Big r = cephyr_big(1, 2, 3, 4, 5, 6, b, 0xf0);
    if (r.a != 391 || r.b != 0x53 || r.c != 791 || b.a != 300 || b.b != 0xa3 || b.c != 400)
        return 25;
    if (big_target(1, 2, 3, 4, 5, 6, b, 0xf0).c != 791 || b.c != 400)
        return 26;
    if (cephyr_noargs().b[16] != 4)
        return 27;
    struct TwoWords seven = cephyr_seven(1, 2, 3, 4, 5, 6, 7);
    if (seven.lo != 22 || seven.hi != 78)
        return 28;
    if (cephyr_split_scalar(1, 2, 3, 4, 5, 6, 7, seven, 4) != 132)
        return 29;
    return cephyr_wide_case(clang_nine, clang_big);
}

int test_main(void) {
    struct Three x = {{131, 255, 7}};
    struct Three a = cephyr_three(x, 19);
    if (a.b[0] != 150 || a.b[1] != 255 || a.b[2] != 248 || x.b[0] != 131)
        return 11;
    if (target(x, 2).b[0] != 133)
        return 12;
    struct Padded p = {253, 0x87654321u};
    struct Padded p2 = cephyr_padded(p);
    if (p2.tag != 11 || p2.n != 0x48664120u || p.n != 0x87654321u)
        return 13;
    union Word u = {.n = 0xfedcba9876543210ul};
    if (cephyr_union(u).n != 0x0fdeb99c73523518ul)
        return 14;
    int n = 30;
    struct Pointer ptr = {&n};
    if (cephyr_pointer(ptr).p != &n || n != 31)
        return 15;
    struct Nested nest = {{{0x91f0}, {0xaa55}}};
    struct Nested n2 = cephyr_nested(nest);
    if (n2.a[0].n != 0xaa55 || n2.a[1].n != 0x91f0)
        return 16;
    struct Three spill = cephyr_stack(1, 2, 3, 4, 5, 6, 7, 8, x, 44, p);
    if (spill.b[0] != 167 || spill.b[1] != 43 || spill.b[2] != 37)
        return 17;
    const int small = cephyr_case(clang_three);
    return small ? small : wide_test();
}
