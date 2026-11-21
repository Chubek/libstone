struct Pair {
    short x;
    int y;
};

union Body {
    unsigned long word;
    unsigned char bytes[9];
};

struct Packet {
    char kind;
    struct Pair rows[2];
    union Body payload;
    const char label[4];
    int *ref;
};

_Static_assert(sizeof(struct Pair) == 8, "member and trailing padding");
_Static_assert(_Alignof(struct Pair) == 4, "natural structure alignment");
_Static_assert(sizeof(union Body) == 16, "union size rounds up to its alignment");
_Static_assert(_Alignof(union Body) == 8, "natural union alignment");
_Static_assert(sizeof(struct Packet) == 56, "nested structure layout");
_Static_assert(_Alignof(struct Packet) == 8, "nested structure alignment");

int counter = 42;
struct Packet wire[] = {
    {7, {{2, 3}, {4, 5}}, {0x1122334455667788ul}, "xy", &counter},
    {8, 9, 10, 11, 12, 13, "ab"},
};
int *relocated_member = &wire[1].rows[1].y;
const char *relocated_array = wire[0].label;
struct Packet zeros;
_Static_assert(sizeof wire == 112, "inferred array-of-record bound");

struct Link;
typedef struct Link Link;
static Link *previous;
struct Link {
    char code;
    Link *next;
};
_Static_assert(sizeof(Link) == 16, "completion through earlier typedef");

typedef struct {
    int *p;
    short val;
} anonymous_tag;
struct Mixed { int *pointer, number; };
_Static_assert(sizeof(struct Mixed) == 16, "mixed member declarators");

static int bound_source;
typedef struct { char data[sizeof bound_source]; } Captured;

struct Moves {
    char lo, hi;
    long cells[20];
};
_Static_assert(sizeof(struct Moves) == 168, "multiple member declarators");

int sparse[] = {[sizeof(int) + 2] = 7, 8, [2] = 3, [2] = 12};
_Static_assert(sizeof sparse == 8 * sizeof(int), "largest designated index");
static const int grid[2][3] = {[0][1] = 2, 3, [1] = {[2] = 7}, [0][1] = 9};
static struct Packet named = {
    .rows[1].x = 28, 31, .kind = 't', .rows[0].y = 26,
    .label = "hey", .label[0] = 'H',
    .ref = &counter, .ref = 0,
};
union Selection { int *p; char label[8]; };
extern int deliberately_undefined;
static union Selection selected = {.p = &deliberately_undefined, .label = "x"};
static union Body cleared = {.word = 0xfffffffffffffffful, .bytes[0] = 12};

static struct Pair *pick(struct Pair *base, int *calls) {
    ++*calls;
    return base;
}

int main(void) {
    char bound_source = 0;
    _Static_assert(sizeof(Captured) == sizeof(int), "member bound uses typedef declaration scope");
    if (bound_source)
        return 16;
    const struct Packet *read = &wire[0];
    struct Packet copy = *read;
    if (read->rows[1].x != 4 || copy.rows[0].y != 3 || copy.label[2] ||
        copy.payload.bytes[0] != 0x88 || *copy.ref != 42)
        return 1;
    if ((char *)&wire[0].rows != (char *)&wire[0] + 4 ||
        (char *)&wire[0].payload != (char *)&wire[0] + 24 ||
        (char *)&wire[0].label != (char *)&wire[0] + 40 ||
        (char *)&wire[0].ref != (char *)&wire[0] + 48 ||
        &wire[0].payload.word != (unsigned long *)&wire[0].payload.bytes ||
        &wire[1] - &wire[0] != 1)
        return 2;
    if (*relocated_member != 12 || relocated_array[1] != 'y' ||
        wire[1].ref || zeros.ref || zeros.rows[1].y || zeros.label[3])
        return 3;
    *relocated_member = 61;
    if (wire[1].rows[1].y != 61)
        return 4;

    Link first = {3, previous};
    previous = &first;
    Link second = {4, previous};
    if (second.next->code != 3 || first.next)
        return 5;
    {
        struct Link { short replacement; } other = {27};
        _Static_assert(sizeof(struct Link) == 2, "scoped tag identity");
        _Static_assert(sizeof(Link) == 16, "typedef preserves outer tag");
        if (other.replacement != 27 || second.next->code != 3)
            return 6;
    }

    struct Pair local[] = {1, 2, {3}};
    int calls = 0;
    pick(local, &calls)->y += 5;
    if (calls != 1 || local[0].y != 7 || local[1].y || ++local[1].x != 4)
        return 7;
    anonymous_tag wrap = {&local[1].y, 18};
    *wrap.p = wrap.val;
    if (local[1].y != 18)
        return 8;
    struct Pair saved = calls ? local[0] : local[1];
    if ((local[1] = saved).y != 7 || local[1].x != 1 ||
        (local[0], local[1]).x != 1)
        return 9;
    union Body u = {0}, v = {0x1122334455667788ul};
    u = v;
    if (u.word != v.word || u.bytes[2] != 0x66)
        return 10;
    u.word = 0;
    u.bytes[0] = 8;
    if (u.word != 8)
        return 11;

    struct Moves moves = {'a', 'b', {1, 2, 3}};
    moves.cells[19] = -15;
    struct Moves old = moves;
    moves.hi = '?';
    moves.cells[19] = 9;
    if (old.hi != 'b' || old.cells[19] != -15 || old.cells[18] ||
        (calls ? old : moves).cells[19] != -15)
        return 12;

    static struct Packet inner = {6, {{3, 5}}, {41}, "z"};
    static const int *inner_alias = &inner.rows[0].y;
    if (*inner_alias != 5 || inner.payload.word != 41 || inner.label[1])
        return 13;
    if (sparse[0] || sparse[2] != 12 || sparse[5] || sparse[6] != 7 || sparse[7] != 8 ||
        grid[0][0] || grid[0][1] != 9 || grid[0][2] != 3 || grid[1][2] != 7 ||
        named.rows[1].x != 28 || named.rows[1].y != 31 ||
        named.rows[0].x || named.rows[0].y != 26 || named.kind != 't' ||
        named.label[0] != 'H' || named.label[1] != 'e' || named.ref ||
        selected.label[0] != 'x' || selected.label[7] || cleared.word != 12)
        return 14;
    struct Pair updates[4] = {
        [0] = saved, [1] = {3, 4}, [1] = {5}, [3].x = 9, 17, [3].x = 6,
    };
    if (updates[0].y != 7 || updates[1].x != 5 || updates[1].y || updates[2].y ||
        updates[3].x != 6 || updates[3].y != 17)
        return 15;
    return 0;
}
