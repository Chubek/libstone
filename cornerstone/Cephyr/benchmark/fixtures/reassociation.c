/* reassociation.c - Modular integer reassociation with constant operands.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_reassociation(unsigned long x, unsigned long y)
{
    unsigned long a = ((x + 17UL) + 23UL) + 41UL;
    unsigned long b = (y * 3UL) * 5UL;
    unsigned long c = (x ^ 0x55UL) ^ 0xa5UL;
    unsigned long d = (y & 0xfff0UL) & 0x0ff0UL;
    return (a ^ b) + (c ^ d);
}

int main(void)
{
    if (bench_reassociation(0x0000000000000000UL, 0x0000000000000000UL) != 0x0000000000000141UL) return 1;
    if (bench_reassociation(0x0000000000000001UL, 0x0000000000000001UL) != 0x000000000000014eUL) return 2;
    if (bench_reassociation(0x0000000000000002UL, 0x0000000000000003UL) != 0x0000000000000170UL) return 3;
    if (bench_reassociation(0x0000000000000007UL, 0x0000000000000007UL) != 0x0000000000000128UL) return 4;
    if (bench_reassociation(0x8000000000000000UL, 0x8000000000000001UL) != 0x800000000000014eUL) return 5;
    if (bench_reassociation(0xffffffffffffffffUL, 0xfedcba9876543210UL) != 0xeeeeeeeeeeeeebbfUL) return 6;
    if (bench_reassociation(0x9e3779b97f4a7c15UL, 0x123456789abcdef0UL) != 0x2d5de261eda5de8bUL) return 7;
    return 0;
}
