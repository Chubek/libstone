#include "shared.h"

struct Three cephyr_three(struct Three x, unsigned int y) {
    x.b[0] += y;
    x.b[2] ^= x.b[1];
    return x;
}

struct Padded cephyr_padded(struct Padded x) {
    x.tag += 14;
    x.n ^= 0xcf030201u;
    return x;
}

union Word cephyr_union(union Word x) {
    x.n ^= 0xf102030405060708ul;
    return x;
}

struct Pointer cephyr_pointer(struct Pointer x) {
    ++*x.p;
    return x;
}

struct Nested cephyr_nested(const struct Nested x) {
    struct Nested r = {{{x.a[1].n}, {x.a[0].n}}};
    return r;
}

struct Three cephyr_stack(unsigned long a, unsigned long b, unsigned long c, unsigned long d,
                          unsigned long e, unsigned long f, unsigned long g, unsigned long h,
                          struct Three x, unsigned long y, struct Padded z) {
    x.b[0] += a + b + c + d + e + f + g + h;
    x.b[1] += y;
    x.b[2] += z.n + z.tag;
    return x;
}

int cephyr_case(ThreeFn *callback) {
    struct Three x = {{131, 255, 7}};
    struct Three a = clang_three(x, 19);
    if (a.b[0] != 131 || a.b[1] != 18 || a.b[2] != 248 || x.b[1] != 255)
        return 1;
    if (callback(x, 2).b[1] != 1)
        return 2;
    struct Padded p = {253, 0x87654321u};
    struct Padded p2 = clang_padded(p);
    if (p2.tag != 252 || p2.n != 0x97654321u || p.n != 0x87654321u)
        return 3;
    union Word u = {.n = 0xfedcba9876543210ul};
    if (clang_union(u).n != 0xeffe89dc23324598ul)
        return 4;
    int n = 30;
    struct Pointer ptr = {&n};
    if (clang_pointer(ptr).p != &n || n != 29)
        return 5;
    struct Nested nest = {{{0x91f0}, {0xaa55}}};
    struct Nested n2 = clang_nested(nest);
    if (n2.a[0].n != 0x6e0f || n2.a[1].n != 0xaa55)
        return 6;
    struct Three spill = clang_stack(1, 2, 3, 4, 5, 6, 7, 8, x, 44, p);
    if (spill.b[0] != 167 || spill.b[1] != 43 || spill.b[2] != 37)
        return 7;
    return 0;
}

struct Nine cephyr_nine(struct Nine x, unsigned long y) {
    x.b[0] += y;
    x.b[8] ^= x.b[7];
    return x;
}

static struct Nine local_nine(void) {
    struct Nine x = {{213, 2, 3, 4, 5, 6, 7, 8, 242}};
    return x;
}

struct TwoWords cephyr_seven(SEVEN) {
    struct TwoWords x = {a + b + 5*c + d, e + 11*f + g};
    return x;
}

unsigned long cephyr_split_scalar(SEVEN, struct TwoWords x, unsigned long tail) {
    return x.lo + x.hi + tail + a + b + c + d + e + f + g;
}

struct TwoWords cephyr_split(SEVEN, struct TwoWords x, unsigned long tail) {
    x.lo += a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g;
    x.hi ^= tail;
    return x;
}

struct Nine cephyr_stacked(SEVEN, unsigned long h, struct Nine x, unsigned long tail) {
    x.b[0] += a + b + c + d + e + f + g + h;
    x.b[8] += tail;
    return x;
}

struct Big cephyr_big(unsigned long a, unsigned long b, unsigned long c, unsigned long d,
                      unsigned long e, unsigned long f, struct Big x, unsigned long tail) {
    x.a += a + 2*b + 3*c + 4*d + 5*e + 6*f;
    x.b ^= tail;
    x.c += x.a;
    return x;
}

struct Seventeen cephyr_noargs(void) {
    struct Seventeen x = {{[0] = 3, [8] = 1, [16] = 4}};
    return x;
}

int cephyr_wide_case(NineFn *nine, BigFn *big) {
    struct Nine x = local_nine();
    if (x.b[0] != 213 || x.b[8] != 242)
        return 31;
    struct Nine y = clang_nine(x, 18);
    if (y.b[0] != 195 || y.b[7] != 8 || y.b[8] != 9)
        return 32;
    if (nine(x, 19).b[0] != 194 || x.b[8] != 242)
        return 33;
    struct TwoWords w = {0x123456789abcdef0ul, 0xf000000000000050ul};
    struct TwoWords z = clang_split(1, 2, 3, 4, 5, 6, 7, w, 32);
    if (z.lo != 0x123456789abcdf7cul || z.hi != 0xf000000000000030ul)
        return 34;
    y = clang_stacked(1, 2, 3, 4, 5, 6, 7, 8, x, 7);
    if (y.b[0] != 177 || y.b[8] != 235)
        return 35;
    struct Big b = {300, 0xa3, 400};
    struct Big r = clang_big(1, 2, 3, 4, 5, 6, b, 0xf0);
    if (r.a != 209 || r.b != 0x53 || r.c != 609 || b.a != 300 || b.b != 0xa3 || b.c != 400)
        return 36;
    if (big(1, 2, 3, 4, 5, 6, b, 0xf0).c != 609 || b.c != 400)
        return 37;
    if (clang_noargs().b[16] != 6)
        return 38;
    struct TwoWords seven = clang_seven(1, 2, 3, 4, 5, 6, 7);
    if (seven.lo != 22 || seven.hi != 78)
        return 39;
    if (clang_split_scalar(1, 2, 3, 4, 5, 6, 7, seven, 4) != 132)
        return 40;
    return 0;
}
