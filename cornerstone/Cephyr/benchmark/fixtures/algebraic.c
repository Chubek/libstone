/* algebraic.c - Classical algebraic identities and cancellation.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_algebraic(unsigned long x, unsigned long y)
{
    unsigned long a = (x + 0UL) ^ 0UL;
    unsigned long b = (1UL * y) - 0UL;
    unsigned long c = (a | 0UL) & ~0UL;
    unsigned long d = (b & ~0UL) + 0UL;
    return (c ^ d) + ((x ^ x) | (y & 0UL));
}

int main(void)
{
    if (bench_algebraic(0x0000000000000000UL, 0x0000000000000000UL) != 0x0000000000000000UL) return 1;
    if (bench_algebraic(0x0000000000000001UL, 0x0000000000000001UL) != 0x0000000000000000UL) return 2;
    if (bench_algebraic(0x0000000000000002UL, 0x0000000000000003UL) != 0x0000000000000001UL) return 3;
    if (bench_algebraic(0x0000000000000007UL, 0x0000000000000007UL) != 0x0000000000000000UL) return 4;
    if (bench_algebraic(0x8000000000000000UL, 0x8000000000000001UL) != 0x0000000000000001UL) return 5;
    if (bench_algebraic(0xffffffffffffffffUL, 0xfedcba9876543210UL) != 0x0123456789abcdefUL) return 6;
    if (bench_algebraic(0x9e3779b97f4a7c15UL, 0x123456789abcdef0UL) != 0x8c032fc1e5f6a2e5UL) return 7;
    return 0;
}
