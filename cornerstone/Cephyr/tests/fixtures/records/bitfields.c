#include "bitfields.h"

struct Full { unsigned a : 32; signed b : 32; };
struct Promoted {
    union {
        struct { unsigned : 1; unsigned count : 7; signed diff : 9; };
        unsigned raw;
    };
    char last;
};

struct Flexible { unsigned count : 4; unsigned char bytes[]; };
_Static_assert(offsetof(struct Flexible, bytes) == 1, "flexible array follows occupied bits");

static struct MixedBits global = {
    .prefix = 0xc5, .small = 2, .delta = -257, .flag = 9,
    .trailer = 0xaa, .small = 5, .wide = 0x71234567u, .sign = -1, .last = 0xbb,
};
static struct MixedBits zero;
static union UnnamedUnion first_named = {0x19};
static struct Promoted promoted = {.raw = 0xffffffffu, .count = 67, -123, 9};
static union {
    const void *ptr;
    struct { unsigned a : 3, b : 3; };
} switched = {.ptr = &global, .a = 2, .b = 5};

_Static_assert(_Generic(+global.small, int: 1, default: 0), "small unsigned bit-field promotes");
_Static_assert(_Generic(global.small, unsigned int: 1, default: 0), "declared member type");
_Static_assert(_Generic(+((struct Full *)0)->a, unsigned int: 1, default: 0), "full unsigned range");
_Static_assert(sizeof((global.small = 3)) == sizeof(unsigned int), "assignment is not a bit-field");

static struct MixedBits *once(struct MixedBits *p, int *visits) {
    ++*visits;
    return p;
}

int cephyr_bits(void) {
    const unsigned char *bytes = (const unsigned char *)&global;
    if (bytes[0] != 0xc5 || bytes[1] != 0xfd || (bytes[2] & 0x3f) != 0x37 ||
        bytes[3] != 0xaa || bytes[4] != 0x67 || bytes[5] != 0x45 ||
        bytes[6] != 0x23 || bytes[7] != 0xf1 || bytes[8] != 0xbb)
        return 1;
    if (global.small != 5 || global.delta != -257 || !global.flag ||
        global.wide != 0x71234567u || global.sign != -1 ||
        zero.small || zero.delta || zero.flag || zero.wide)
        return 2;
    if (first_named.tag != 0x19 || promoted.count != 67 || promoted.diff != -123 || promoted.last != 9 ||
        switched.a != 2 || switched.b != 5)
        return 3;
    struct MixedBits local = { 0x44, 6, -23, 7, 0xa1, 0x67123456u, -1, 0xb2 };
    if (local.prefix != 0x44 || local.small != 6 || local.delta != -23 || !local.flag ||
        local.trailer != 0xa1 || local.wide != 0x67123456u || local.sign != -1 || local.last != 0xb2)
        return 4;
    if (local.small - 7 >= 0)
        return 5;
    int visits = 0;
    if ((once(&local, &visits)->small += 3) != 1 || visits != 1 || local.delta != -23 || !local.flag)
        return 6;
    if ((local.small = 7) != 7 || ++local.small != 0 || local.small++ != 0 ||
        local.small != 1 || --local.small != 0)
        return 7;
    if ((local.flag = 2) != 1 || (local.flag = 0) != 0 || local.delta != -23)
        return 8;
    struct MixedBits copied = local;
    if ((copied = global).delta != -257 || copied.small != 5)
        return 9;
    union UnnamedUnion local_union = {0x42};
    struct Promoted p = {.count = 121, -3, 18};
    if (local_union.tag != 0x42 || p.count != 121 || p.diff != -3 || p.last != 18)
        return 10;
    struct Full full = {0xffffffffu, -2147483647 - 1};
    if (full.a != 0xffffffffu || full.b != -2147483647 - 1)
        return 11;
    if (external_bits.small != 5 || external_bits.delta != -313 || !external_bits.flag ||
        external_bits.wide != 0x71020408u || external_bits.sign != -1)
        return 12;
    external_update(&global);
    if (global.small != 3 || global.delta != -321 || global.flag ||
        global.wide != 0x10002000u || global.sign != -1 ||
        global.trailer != 0xaa || global.last != 0xbb)
        return 13;
    static struct { const unsigned a : 3; signed b : 2; } block = {6, -2};
    if (block.a != 6 || block.b != -2)
        return 14;
    return 0;
}
