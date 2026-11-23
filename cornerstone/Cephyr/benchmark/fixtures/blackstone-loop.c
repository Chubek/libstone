/* Keep the recurrence in a separate TU from the timed driver. */
unsigned long kernel(unsigned long seed, unsigned long mask)
{
    unsigned long x = seed;
    for (int i = 0; i < 1000; ++i) {
        x = ((x ^ mask) ^ mask) + ((mask & 0UL) | 17UL);
        x = ((x - 0UL) ^ 0UL) * 3UL;
    }
    return x;
}
