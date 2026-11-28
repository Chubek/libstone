/* join_disagree.c - control for cross-block constant propagation.
 * The two branches MUST disagree: a pass that folds x*parameter using one
 * arm's constant silently changes the result, so main returns nonzero. */
unsigned long bench_join_disagree(unsigned long x, unsigned long selector)
{
    unsigned long k;
    if (selector & 1UL)
        k = 3UL;
    else
        k = 5UL;
    return x * k;
}

int main(void)
{
    if (bench_join_disagree(0x10UL, 0UL) != 0x50UL) return 1;
    if (bench_join_disagree(0x10UL, 1UL) != 0x30UL) return 2;
    if (bench_join_disagree(0x0fffffffffffffffUL, 0UL) != 0x4ffffffffffffffbUL) return 3;
    if (bench_join_disagree(0x0fffffffffffffffUL, 1UL) != 0x2ffffffffffffffdUL) return 4;
    return 0;
}
