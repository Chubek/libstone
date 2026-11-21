#include <stddef.h>

struct Record {
    char prefix;
    union {
        long raw;
        struct {
            int x;
            struct { short y, z; };
        };
    };
    int tail;
};

/* Physical nesting, including the padding of the anonymous union. */
_Static_assert(sizeof(struct Record) == 24, "size");
_Static_assert(_Alignof(struct Record) == 8, "alignment");
_Static_assert(offsetof(struct Record, raw) == 8, "union offset");
_Static_assert(offsetof(struct Record, x) == 8, "first struct member");
_Static_assert(offsetof(struct Record, y) == 12, "recursive member offset");
_Static_assert(offsetof(struct Record, z) == 14, "recursive member offset");
_Static_assert(offsetof(struct Record, tail) == 16, "tail offset");

struct Aligned {
    char first;
    _Alignas(16) struct { int promoted; };
};
_Static_assert(offsetof(struct Aligned, promoted) == 16, "member alignment");
_Static_assert(sizeof(struct Aligned) == 32, "trailing padding");

struct Flexible {
    union { int count; };
    char data[];
};
_Static_assert(offsetof(struct Flexible, data) == 4, "promoted named member");

static struct Record state = { .raw = -1, .x = 51, .y = 52, 53, 54 };
static short *saved = &state.z;
static short *also_saved = &(&state)->y;
static const struct { const struct { int value; }; } nested_const = { .value = 7 };

union Pair {
    struct { int left, right; };
    long raw;
};
static union Pair pair = { 11, 12 };

struct Relocation {
    union { short *pointer; struct { int number; }; };
    short *next;
};
static struct Relocation relocation = { .pointer = &state.y, .number = 42, &state.z };

struct Volatile {
    volatile struct { int first, last; };
};

int main(void) {
    if (state.prefix || state.x != 51 || state.y != 52 ||
        *saved != 53 || *also_saved != 52 || state.tail != 54)
        return 1;
    if (pair.left != 11 || pair.right != 12 || nested_const.value != 7)
        return 2;
    if (relocation.number != 42 || relocation.next != &state.z)
        return 6;

    struct Record ordered = { 1, { .x = 2, .y = 3, 4 }, 5 };
    struct Record local = { .x = 10, .z = 12, 13 };
    struct Record *p = &local;
    p->y = ordered.y;
    short *z = &p->z;
    ++*z;
    if (ordered.prefix != 1 || ordered.x != 2 || ordered.z != 4 || ordered.tail != 5 ||
        local.x != 10 || local.y != 3 || local.z != 13 || local.tail != 13)
        return 3;
    struct Record copy = *p;
    if ((copy = ordered).z != 4 || copy.x != 2)
        return 4;

    struct Volatile v = { .last = 8 };
    if (v.first || v.last != 8)
        return 5;
    return 0;
}
