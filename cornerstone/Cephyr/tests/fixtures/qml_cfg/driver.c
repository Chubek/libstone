unsigned long cfg_walk(volatile unsigned long *cell, long bound, unsigned int steps);
long cfg_signed_control(long bound);
unsigned long cfg_unsigned_control(unsigned long x, unsigned long divisor_bits, long bound);
long cfg_write_stdout(const void *data, unsigned long size);

enum { Walk, Cell, Calls, Signed, Unsigned, Samples, MaxSteps = 8 };

struct input {
    unsigned long cell;
    long bound;
    unsigned long divisor_bits;
    unsigned int steps;
};

static const struct input cases[] = {
#include "inputs.inc"
};

static unsigned long observed[sizeof cases / sizeof cases[0]][Samples + MaxSteps];
static unsigned long *active;
static unsigned long calls;

unsigned long cfg_observe(volatile unsigned long *cell, unsigned int index, long bound) {
    unsigned long seen = *cell;
    if (calls < MaxSteps)
        active[Samples + calls] = seen;
    ++calls;
    unsigned long updated = (seen >> 1) ^ (unsigned long)bound ^ ((unsigned long)index << 60);
    *cell = updated;
    return seen + updated;
}

int main(void) {
    for (unsigned int i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const struct input *test = &cases[i];
        volatile unsigned long cell = test->cell;
        active = observed[i];
        calls = 0;
        active[Walk] = cfg_walk(&cell, test->bound, test->steps);
        active[Cell] = cell;
        active[Calls] = calls;
        active[Signed] = (unsigned long)cfg_signed_control(test->bound);
        active[Unsigned] = cfg_unsigned_control(test->cell, test->divisor_bits, test->bound);
    }
    /* Every supported target is little-endian LP64. Write one fixed-size record per case. */
    return cfg_write_stdout(observed, sizeof observed) != sizeof observed;
}
