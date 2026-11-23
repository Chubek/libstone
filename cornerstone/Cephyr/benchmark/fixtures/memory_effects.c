/* memory_effects.c - Volatile reads/writes and side-effecting calls must stay observable.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

static volatile unsigned long cell;
static volatile unsigned long calls;
unsigned long touch(unsigned long x)
{
    calls = calls + 1UL;
    return x ^ calls;
}
unsigned long bench_memory_effects(unsigned long x)
{
    unsigned long a = cell;
    cell = a + 1UL;
    unsigned long b = cell;
    unsigned long c = touch(x);
    unsigned long d = touch(x);
    return (a ^ b) + (c ^ d) + cell;
}

int main(void)
{
    cell = 0x0000000000000000UL; calls = 0UL;
    if (bench_memory_effects(0x0000000000000000UL) != 0x0000000000000005UL || cell != 0x0000000000000001UL || calls != 2UL) return 1;
    cell = 0x0000000000000001UL; calls = 0UL;
    if (bench_memory_effects(0x0000000000000001UL) != 0x0000000000000008UL || cell != 0x0000000000000002UL || calls != 2UL) return 2;
    cell = 0x0000000000000003UL; calls = 0UL;
    if (bench_memory_effects(0x0000000000000002UL) != 0x000000000000000eUL || cell != 0x0000000000000004UL || calls != 2UL) return 3;
    cell = 0x0000000000000007UL; calls = 0UL;
    if (bench_memory_effects(0x0000000000000007UL) != 0x000000000000001aUL || cell != 0x0000000000000008UL || calls != 2UL) return 4;
    cell = 0x8000000000000001UL; calls = 0UL;
    if (bench_memory_effects(0x8000000000000000UL) != 0x8000000000000008UL || cell != 0x8000000000000002UL || calls != 2UL) return 5;
    cell = 0xfedcba9876543210UL; calls = 0UL;
    if (bench_memory_effects(0xffffffffffffffffUL) != 0xfedcba9876543215UL || cell != 0xfedcba9876543211UL || calls != 2UL) return 6;
    cell = 0x123456789abcdef0UL; calls = 0UL;
    if (bench_memory_effects(0x9e3779b97f4a7c15UL) != 0x123456789abcdef5UL || cell != 0x123456789abcdef1UL || calls != 2UL) return 7;
    return 0;
}
