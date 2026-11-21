#include <stdlib.h>

typedef unsigned long Words[];
struct Wide {
    unsigned char kind;
    Words words;
};

struct Slack {
    long key;
    char kind;
    unsigned char tail[];
};

struct Grid {
    int rows;
    int cells[][3];
};

union Container {
    struct Wide header;
    long alternative;
};

union Nested {
    union Container box;
    long alternative;
};

struct References {
    struct Wide *header;
    union Nested *box;
};

_Static_assert(sizeof(struct Wide) == 8, "tail alignment may enlarge the header");
_Static_assert(_Alignof(struct Wide) == 8, "tail element alignment");
_Static_assert(sizeof(struct Slack) == 16, "flexible tail can begin in padding");
_Static_assert(sizeof(struct Grid) == 4, "flexible array of complete arrays");
_Static_assert(sizeof(union Nested) == 8, "unions may recursively contain a flexible structure");

static union Nested nested = {13};
static struct Slack pad = {.key = 5, 's'};
static const unsigned char *tail_at_link_time = pad.tail;
static unsigned char (*whole_tail_at_link_time)[] = &pad.tail;
static struct Wide empty = {3};
static struct References refs[2] = {{&empty, &nested}};

static unsigned long sum(const struct Wide *p) {
    _Static_assert(_Generic(p->words, const unsigned long *: 1, default: 0),
                   "flexible member decay preserves the record's qualification");
    const unsigned long *first = p->words;
    const unsigned long *end = first + 3;
    return first[0] + first[1] + *(end - 1);
}

int main(void) {
    if (nested.box.header.kind != 13 || pad.key != 5 || pad.kind != 's' ||
        refs[0].header->kind != 3 || refs[0].box->box.header.kind != 13 ||
        refs[1].header || refs[1].box ||
        tail_at_link_time != *whole_tail_at_link_time ||
        (const char *)tail_at_link_time != (const char *)&pad + 9 ||
        (const char *)empty.words != (const char *)&empty + 8)
        return 1;

    struct Wide *a = malloc(sizeof *a + 3 * sizeof(unsigned long));
    struct Wide *b = malloc(sizeof *b + 3 * sizeof(unsigned long));
    struct Grid *g = malloc(sizeof *g + 2 * sizeof(int[3]));
    if (!a || !b || !g)
        return 2;

    a->kind = 34;
    a->words[0] = 10;
    a->words[1] = 12;
    a->words[2] = 13;
    b->kind = 56;
    b->words[0] = 99;
    b->words[1] = 77;
    b->words[2] = 55;
    if (sum(a) != 35)
        return 3;
    *b = *a; /* Only the fixed header is copied. */
    struct Wide fixed = *b;
    if (fixed.kind != 34 || b->words[0] != 99 ||
        b->words[1] != 77 || b->words[2] != 55)
        return 4;

    g->rows = 2;
    (*g).cells[0][2] = 19;
    g->cells[1][0] = 23;
    int (*row)[3] = g->cells;
    if (sizeof g->cells[0] != 3 * sizeof(int) ||
        g->rows != 2 || row[0][2] != 19 || row[1][0] != 23 ||
        &row[1] - &row[0] != 1)
        return 5;
    free(g);
    free(b);
    free(a);
    return 0;
}
