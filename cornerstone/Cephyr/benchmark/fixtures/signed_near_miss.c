/* signed_near_miss.c - Signed division/remainder: do not apply unsigned identities.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_signed_near_miss(long x, long y)
{
    long rem = x % y;
    long quotient = x / y;
    return (((unsigned long)rem) ^ ((unsigned long)quotient)) + (x < y ? 1UL : 2UL);
}

int main(void)
{
    if (bench_signed_near_miss((-98765L), 3L) != 0x000000000000809aUL) return 1;
    if (bench_signed_near_miss((-7L), 3L) != 0x0000000000000002UL) return 2;
    if (bench_signed_near_miss((-1L), 7L) != 0x0000000000000000UL) return 3;
    if (bench_signed_near_miss(0L, 7L) != 0x0000000000000001UL) return 4;
    if (bench_signed_near_miss(1L, 3L) != 0x0000000000000002UL) return 5;
    if (bench_signed_near_miss(98765L, 7L) != 0x0000000000003721UL) return 6;
    if (bench_signed_near_miss((-9223372036854775807L), 11L) != 0x0ba2e8ba2e8ba2e2UL) return 7;
    return 0;
}
