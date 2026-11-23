/* sandslab_literals.c - Narrow literal materialization across volatile accesses.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

static volatile unsigned long observed;
unsigned long bench_sandslab_literals(unsigned long x)
{
    observed = x + 0x0000000000000000UL;
    x = observed ^ (x >> 3);
    observed = x + 0x0000000000000001UL;
    x = observed ^ (x >> 3);
    observed = x + 0x000000000000007fUL;
    x = observed ^ (x >> 3);
    observed = x + 0x00000000000000ffUL;
    x = observed ^ (x >> 3);
    observed = x + 0x000000000000ffffUL;
    x = observed ^ (x >> 3);
    observed = x + 0x0000000012345678UL;
    x = observed ^ (x >> 3);
    observed = x + 0x000000007fffffffUL;
    x = observed ^ (x >> 3);
    observed = x + 0x0000000080000000UL;
    x = observed ^ (x >> 3);
    observed = x + 0x00000000ffffffffUL;
    x = observed ^ (x >> 3);
    return x;
}

int main(void)
{
    if (bench_sandslab_literals(0x0000000000000000UL) != 0x0000000222321602UL) return 1;
    if (bench_sandslab_literals(0x0000000000000001UL) != 0x000000022232185cUL) return 2;
    if (bench_sandslab_literals(0x0000000000000002UL) != 0x0000000222321875UL) return 3;
    if (bench_sandslab_literals(0x0000000000000007UL) != 0x0000000222321871UL) return 4;
    if (bench_sandslab_literals(0x8000000000000000UL) != 0x9000009222321602UL) return 5;
    if (bench_sandslab_literals(0xffffffffffffffffUL) != 0xe00000e222321602UL) return 6;
    if (bench_sandslab_literals(0x9e3779b97f4a7c15UL) != 0x8df19602e2bc6ce7UL) return 7;
    return 0;
}
