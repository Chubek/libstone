/* sandslab_wide.c - Wide immediates and relocatable data address.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

static volatile unsigned long slot;
unsigned long bench_sandslab_wide(unsigned long x)
{
    slot = x + 0x100000000UL;
    return (slot ^ 0xffffffffffffffffUL) + 0x8000000000000000UL;
}

int main(void)
{
    if (bench_sandslab_wide(0x0000000000000000UL) != 0x7ffffffeffffffffUL) return 1;
    if (bench_sandslab_wide(0x0000000000000001UL) != 0x7ffffffefffffffeUL) return 2;
    if (bench_sandslab_wide(0x0000000000000002UL) != 0x7ffffffefffffffdUL) return 3;
    if (bench_sandslab_wide(0x0000000000000007UL) != 0x7ffffffefffffff8UL) return 4;
    if (bench_sandslab_wide(0x8000000000000000UL) != 0xfffffffeffffffffUL) return 5;
    if (bench_sandslab_wide(0xffffffffffffffffUL) != 0x7fffffff00000000UL) return 6;
    if (bench_sandslab_wide(0x9e3779b97f4a7c15UL) != 0xe1c8864580b583eaUL) return 7;
    return 0;
}
