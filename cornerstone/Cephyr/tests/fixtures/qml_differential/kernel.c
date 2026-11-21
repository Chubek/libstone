unsigned long observe(unsigned long *cell, unsigned int tag);

long signed64(long x) {
    return (0L + x) * 1L - 0L;
}

int signed32(int x) {
    return (x + 0) * 1 - 0;
}

unsigned long unsigned64(unsigned long *cell, unsigned int tag) {
    unsigned long loaded = *cell;
    *cell = ((loaded + 0UL) * 2UL) ^ 0UL;
    unsigned long from_call = 2UL * (observe(cell, tag) | 0UL);
    return (from_call + *cell + loaded % (1UL << 63)) - 0UL;
}

unsigned int unsigned32(unsigned int x) {
    return ((2U * x) << 0) | 0U;
}
