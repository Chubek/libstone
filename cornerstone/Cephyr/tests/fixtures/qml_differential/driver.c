long signed64(long x);
int signed32(int x);
unsigned long unsigned64(unsigned long *cell, unsigned int tag);
unsigned int unsigned32(unsigned int x);

static unsigned int calls;
static unsigned long seen;

unsigned long observe(unsigned long *cell, unsigned int tag) {
    ++calls;
    seen = *cell;
    *cell = (seen >> 1) ^ (unsigned long)tag;
    return seen + tag;
}

struct inputs {
    long s64;
    int s32;
    unsigned long u64;
    unsigned int u32;
    unsigned int doubled32;
    unsigned long result;
    unsigned long cell;
    unsigned long seen;
};

static const struct inputs cases[] = {
#include "inputs.inc"
};

int main(void) {
    for (unsigned int i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const struct inputs *test = &cases[i];
        unsigned long cell = test->u64;
        unsigned long result = unsigned64(&cell, test->u32);
        /* Each mismatch identifies both the case and the observed field. */
        if (signed64(test->s64) != test->s64) return 7 * i + 1;
        if (signed32(test->s32) != test->s32) return 7 * i + 2;
        if (unsigned32(test->u32) != test->doubled32) return 7 * i + 3;
        if (result != test->result) return 7 * i + 4;
        if (cell != test->cell) return 7 * i + 5;
        if (seen != test->seen) return 7 * i + 6;
        if (calls != i + 1) return 7 * i + 7;
    }
    return 0;
}
