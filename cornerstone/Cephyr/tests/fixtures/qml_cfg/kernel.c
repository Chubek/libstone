unsigned long cfg_observe(volatile unsigned long *cell, unsigned int index, long bound);

unsigned long cfg_walk(volatile unsigned long *cell, long bound, unsigned int steps) {
    unsigned long acc = *cell;
    for (unsigned int i = 0; i < steps; ++i) {
        unsigned long previous = *cell;
        unsigned long next;
        if (bound < (long)i - 3L)
            next = ((previous + 0UL) * 2UL) ^ 0UL;
        else
            next = previous % (1UL << 63);

        if (i & 1U) {
            unsigned long payload = (next & 255UL) & 65535UL;
            unsigned long marker = 256UL | (previous & 256UL);
            /* The AND is always zero, but the volatile read must remain. */
            unsigned long empty = (*cell & 255UL) & 256UL;
            next = (payload | marker) ^ empty;
        }
        *cell = next;
        acc ^= cfg_observe(cell, i, bound);
        acc += *cell;
    }
    return acc;
}

long cfg_signed_control(long bound) {
    return bound % 256L;
}

unsigned long cfg_unsigned_control(unsigned long x, unsigned long divisor_bits, long bound) {
    unsigned long divisor;
    if (bound < 0L)
        divisor = 1UL << 63;
    else
        divisor = divisor_bits | 1UL;

    /* The masks overlap, and only one branch has a known power-of-two divisor. */
    return ((x & 255UL) & (divisor_bits & 128UL)) ^ (x % 255UL) ^ (x % divisor);
}
