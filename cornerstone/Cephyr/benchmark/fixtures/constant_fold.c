/* constant_fold.c - Fold constants introduced by simple definitions.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_constant_fold(unsigned long x)
{
    unsigned long a = 17UL;
    unsigned long b = 5UL;
    unsigned long c = (a + b) * (a ^ b);
    unsigned long d = ((17UL << 3) | (255UL & 127UL));
    return x ^ (c + d);
}

int main(void)
{
    if (bench_constant_fold(0x0000000000000000UL) != 0x00000000000002b7UL) return 1;
    if (bench_constant_fold(0x0000000000000001UL) != 0x00000000000002b6UL) return 2;
    if (bench_constant_fold(0x0000000000000002UL) != 0x00000000000002b5UL) return 3;
    if (bench_constant_fold(0x0000000000000007UL) != 0x00000000000002b0UL) return 4;
    if (bench_constant_fold(0x8000000000000000UL) != 0x80000000000002b7UL) return 5;
    if (bench_constant_fold(0xffffffffffffffffUL) != 0xfffffffffffffd48UL) return 6;
    if (bench_constant_fold(0x9e3779b97f4a7c15UL) != 0x9e3779b97f4a7ea2UL) return 7;
    return 0;
}
