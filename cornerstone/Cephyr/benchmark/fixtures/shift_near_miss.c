/* shift_near_miss.c - Different logical shift counts from runtime operands.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_shift_near_miss(unsigned long x, unsigned long y)
{
    unsigned long k = (y >> 58) & 63UL;
    unsigned long a = x >> k;
    unsigned long b = x << ((k + 1UL) & 63UL);
    return (a ^ b) + (x & y);
}

int main(void)
{
    if (bench_shift_near_miss(0x0000000000000000UL, 0x0000000000000000UL) != 0x0000000000000000UL) return 1;
    if (bench_shift_near_miss(0x0000000000000001UL, 0x0000000000000001UL) != 0x0000000000000004UL) return 2;
    if (bench_shift_near_miss(0x0000000000000002UL, 0x0000000000000003UL) != 0x0000000000000008UL) return 3;
    if (bench_shift_near_miss(0x0000000000000007UL, 0x0000000000000007UL) != 0x0000000000000010UL) return 4;
    if (bench_shift_near_miss(0x8000000000000000UL, 0x8000000000000001UL) != 0x8000000080000000UL) return 5;
    if (bench_shift_near_miss(0xffffffffffffffffUL, 0xfedcba9876543210UL) != 0xfedcba987654320eUL) return 6;
    if (bench_shift_near_miss(0x9e3779b97f4a7c15UL, 0x123456789abcdef0UL) != 0xe14090ec98c38171UL) return 7;
    return 0;
}
