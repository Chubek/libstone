/* dynamic_divisor.c - Runtime-dependent nonzero divisor must not become a bit mask.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_dynamic_divisor(unsigned long x, unsigned long y)
{
    unsigned long divisor = y | 1UL;
    unsigned long remainder = x % divisor;
    unsigned long upper = (x & 65535UL) & 32767UL;
    return remainder + (upper ^ (y << 2));
}

int main(void)
{
    if (bench_dynamic_divisor(0x0000000000000000UL, 0x0000000000000000UL) != 0x0000000000000000UL) return 1;
    if (bench_dynamic_divisor(0x0000000000000001UL, 0x0000000000000001UL) != 0x0000000000000005UL) return 2;
    if (bench_dynamic_divisor(0x0000000000000002UL, 0x0000000000000003UL) != 0x0000000000000010UL) return 3;
    if (bench_dynamic_divisor(0x0000000000000007UL, 0x0000000000000007UL) != 0x000000000000001bUL) return 4;
    if (bench_dynamic_divisor(0x8000000000000000UL, 0x8000000000000001UL) != 0x8000000000000004UL) return 5;
    if (bench_dynamic_divisor(0xffffffffffffffffUL, 0xfedcba9876543210UL) != 0xfc962fc962fc85adUL) return 6;
    if (bench_dynamic_divisor(0x9e3779b97f4a7c15UL, 0x123456789abcdef0UL) != 0x55661fd714568c62UL) return 7;
    return 0;
}
