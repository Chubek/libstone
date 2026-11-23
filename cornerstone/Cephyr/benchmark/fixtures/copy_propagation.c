/* copy_propagation.c - Chains of copies, constants and zero-valued additions.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_copy_propagation(unsigned long x, unsigned long y)
{
    unsigned long a = x;
    unsigned long b = a;
    unsigned long c = b;
    unsigned long d = 37UL;
    unsigned long e = d;
    unsigned long f = e + 0UL;
    return (((c + f) ^ d) + f) ^ y;
}

int main(void)
{
    if (bench_copy_propagation(0x0000000000000000UL, 0x0000000000000000UL) != 0x0000000000000025UL) return 1;
    if (bench_copy_propagation(0x0000000000000001UL, 0x0000000000000001UL) != 0x0000000000000029UL) return 2;
    if (bench_copy_propagation(0x0000000000000002UL, 0x0000000000000003UL) != 0x0000000000000024UL) return 3;
    if (bench_copy_propagation(0x0000000000000007UL, 0x0000000000000007UL) != 0x0000000000000029UL) return 4;
    if (bench_copy_propagation(0x8000000000000000UL, 0x8000000000000001UL) != 0x0000000000000024UL) return 5;
    if (bench_copy_propagation(0xffffffffffffffffUL, 0xfedcba9876543210UL) != 0xfedcba9876543236UL) return 6;
    if (bench_copy_propagation(0x9e3779b97f4a7c15UL, 0x123456789abcdef0UL) != 0x8c032fc1e5f6a2b4UL) return 7;
    return 0;
}
