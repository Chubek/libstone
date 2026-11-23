/* loop_induction.c - Induction-value multiply by constant with a single latch.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_loop_induction(unsigned long n, unsigned long seed)
{
    unsigned long result = seed;
    unsigned long i;
    for (i = 0UL; i < n; ++i) {
        result += i * 8UL;
        result ^= i & 1UL;
    }
    return result;
}

int main(void)
{
    if (bench_loop_induction(0x0000000000000000UL, 0x0000000000000000UL) != 0x0000000000000000UL) return 1;
    if (bench_loop_induction(0x0000000000000001UL, 0x0000000000000001UL) != 0x0000000000000001UL) return 2;
    if (bench_loop_induction(0x0000000000000002UL, 0x0000000000000007UL) != 0x000000000000000eUL) return 3;
    if (bench_loop_induction(0x0000000000000007UL, 0x0000000000abcdefUL) != 0x0000000000abce96UL) return 4;
    if (bench_loop_induction(0x0000000000000010UL, 0xffffffffffffffffUL) != 0x00000000000003bfUL) return 5;
    if (bench_loop_induction(0x000000000000001fUL, 0x8000000000000000UL) != 0x8000000000000e89UL) return 6;
    if (bench_loop_induction(0x0000000000000030UL, 0x0000000012345678UL) != 0x00000000123479b8UL) return 7;
    return 0;
}
