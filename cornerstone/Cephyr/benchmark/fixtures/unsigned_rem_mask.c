/* unsigned_rem_mask.c - Unsigned power-of-two remainder and guarded masking.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_unsigned_rem_mask(unsigned long x, unsigned long y)
{
    unsigned long rem = x % (1UL << 8);
    unsigned long a = (x & 255UL) & 65535UL;
    unsigned long b = ((y | 255UL) & (y & 255UL));
    return (rem + a) ^ b;
}

int main(void)
{
    if (bench_unsigned_rem_mask(0x0000000000000000UL, 0x0000000000000000UL) != 0x0000000000000000UL) return 1;
    if (bench_unsigned_rem_mask(0x0000000000000001UL, 0x0000000000000001UL) != 0x0000000000000003UL) return 2;
    if (bench_unsigned_rem_mask(0x0000000000000002UL, 0x0000000000000003UL) != 0x0000000000000007UL) return 3;
    if (bench_unsigned_rem_mask(0x0000000000000007UL, 0x0000000000000007UL) != 0x0000000000000009UL) return 4;
    if (bench_unsigned_rem_mask(0x8000000000000000UL, 0x8000000000000001UL) != 0x0000000000000001UL) return 5;
    if (bench_unsigned_rem_mask(0xffffffffffffffffUL, 0xfedcba9876543210UL) != 0x00000000000001eeUL) return 6;
    if (bench_unsigned_rem_mask(0x9e3779b97f4a7c15UL, 0x123456789abcdef0UL) != 0x00000000000000daUL) return 7;
    return 0;
}
