/* bitwise_factor.c - Absorption, factoring and cancellation in integer bitwise DAGs.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_bitwise_factor(unsigned long x, unsigned long y)
{
    unsigned long z = 0xa5a5a5a5a5a5a5a5UL;
    unsigned long a = (x & y) | (x & z);
    unsigned long b = (x | y) & (x | z);
    unsigned long c = (x ^ y) ^ y;
    unsigned long d = x & (x | z);
    return (a ^ b) + (c ^ d);
}

int main(void)
{
    if (bench_bitwise_factor(0x0000000000000000UL, 0x0000000000000000UL) != 0x0000000000000000UL) return 1;
    if (bench_bitwise_factor(0x0000000000000001UL, 0x0000000000000001UL) != 0x0000000000000000UL) return 2;
    if (bench_bitwise_factor(0x0000000000000002UL, 0x0000000000000003UL) != 0x0000000000000001UL) return 3;
    if (bench_bitwise_factor(0x0000000000000007UL, 0x0000000000000007UL) != 0x0000000000000000UL) return 4;
    if (bench_bitwise_factor(0x8000000000000000UL, 0x8000000000000001UL) != 0x0000000000000001UL) return 5;
    if (bench_bitwise_factor(0xffffffffffffffffUL, 0xfedcba9876543210UL) != 0x00024042080a484aUL) return 6;
    if (bench_bitwise_factor(0x9e3779b97f4a7c15UL, 0x123456789abcdef0UL) != 0x08020c00c0e680a0UL) return 7;
    return 0;
}
