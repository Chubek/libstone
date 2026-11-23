/* unsigned_wrap.c - Unsigned modular wraparound at 64-bit boundary.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_unsigned_wrap(unsigned long x)
{
    unsigned long a = (x + 0xffffffffffffffffUL) + 1UL;
    unsigned long b = (x * 3UL) + (x * 5UL);
    unsigned long c = (x ^ x) + (x - x);
    return (a ^ b) + c;
}

int main(void)
{
    if (bench_unsigned_wrap(0x0000000000000000UL) != 0x0000000000000000UL) return 1;
    if (bench_unsigned_wrap(0x0000000000000001UL) != 0x0000000000000009UL) return 2;
    if (bench_unsigned_wrap(0x0000000000000002UL) != 0x0000000000000012UL) return 3;
    if (bench_unsigned_wrap(0x0000000000000007UL) != 0x000000000000003fUL) return 4;
    if (bench_unsigned_wrap(0x8000000000000000UL) != 0x8000000000000000UL) return 5;
    if (bench_unsigned_wrap(0xffffffffffffffffUL) != 0x0000000000000007UL) return 6;
    if (bench_unsigned_wrap(0x9e3779b97f4a7c15UL) != 0x6f8cb47285199cbdUL) return 7;
    return 0;
}
