/* cse_dominance.c - Dominating expression reused across two branch successors.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

unsigned long bench_cse_dominance(unsigned long x, unsigned long y, unsigned long selector)
{
    unsigned long base = x * 13UL + y;
    unsigned long result;
    if (selector & 1UL) {
        unsigned long branch_value = x * 13UL + y;
        result = branch_value ^ base;
    } else {
        unsigned long branch_value = x * 13UL + y;
        result = branch_value + base;
    }
    return result ^ (x | 0UL);
}

int main(void)
{
    if (bench_cse_dominance(0x0000000000000000UL, 0x0000000000000000UL, 0x0000000000000000UL) != 0x0000000000000000UL) return 1;
    if (bench_cse_dominance(0x0000000000000001UL, 0x0000000000000001UL, 0x0000000000000001UL) != 0x0000000000000001UL) return 2;
    if (bench_cse_dominance(0x0000000000000002UL, 0x0000000000000003UL, 0x0000000000000002UL) != 0x0000000000000038UL) return 3;
    if (bench_cse_dominance(0x0000000000000007UL, 0x0000000000000007UL, 0x0000000000000003UL) != 0x0000000000000007UL) return 4;
    if (bench_cse_dominance(0x8000000000000000UL, 0x8000000000000001UL, 0x0000000000000000UL) != 0x8000000000000002UL) return 5;
    if (bench_cse_dominance(0xffffffffffffffffUL, 0xfedcba9876543210UL, 0x0000000000000002UL) != 0x02468acf13579bf9UL) return 6;
    if (bench_cse_dominance(0x9e3779b97f4a7c15UL, 0x123456789abcdef0UL, 0x0000000000000003UL) != 0x9e3779b97f4a7c15UL) return 7;
    return 0;
}
