/* phi_join.c - Same-constant and distinct-value paths through SSA join parameters.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_phi_join(unsigned long x, unsigned long y, unsigned long selector)
{
    unsigned long a;
    unsigned long b;
    if (selector & 1UL)
        a = x + 7UL;
    else
        a = x + 7UL;
    if (selector & 2UL)
        b = x ^ y;
    else
        b = x | y;
    return a + b;
}

int main(void)
{
    if (bench_phi_join(0x0000000000000000UL, 0x0000000000000000UL, 0x0000000000000000UL) != 0x0000000000000007UL) return 1;
    if (bench_phi_join(0x0000000000000001UL, 0x0000000000000001UL, 0x0000000000000001UL) != 0x0000000000000009UL) return 2;
    if (bench_phi_join(0x0000000000000002UL, 0x0000000000000003UL, 0x0000000000000002UL) != 0x000000000000000aUL) return 3;
    if (bench_phi_join(0x0000000000000007UL, 0x0000000000000007UL, 0x0000000000000003UL) != 0x000000000000000eUL) return 4;
    if (bench_phi_join(0x8000000000000000UL, 0x8000000000000001UL, 0x0000000000000000UL) != 0x0000000000000008UL) return 5;
    if (bench_phi_join(0xffffffffffffffffUL, 0xfedcba9876543210UL, 0x0000000000000002UL) != 0x0123456789abcdf5UL) return 6;
    if (bench_phi_join(0x9e3779b97f4a7c15UL, 0x123456789abcdef0UL, 0x0000000000000003UL) != 0x2a3aa97b65411f01UL) return 7;
    return 0;
}
