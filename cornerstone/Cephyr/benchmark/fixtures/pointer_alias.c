/* pointer_alias.c - Aliased/nonaliased reads across intervening writes.
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */

static unsigned long slots[2];
unsigned long bench_pointer_alias(unsigned long *p, unsigned long *q, unsigned long x)
{
    unsigned long before = *p;
    *q = x;
    unsigned long after = *p;
    return (before ^ after) + *q;
}

int main(void)
{
    slots[0] = 0x0000000000000000UL; slots[1] = 0x000000000000000cUL;
    if (bench_pointer_alias(&slots[0], &slots[0], 0x0000000000000003UL) != 0x0000000000000006UL || slots[0] != 0x0000000000000003UL || slots[1] != 0x000000000000000cUL) return 1;
    slots[0] = 0x0000000000000001UL; slots[1] = 0x0000000000000003UL;
    if (bench_pointer_alias(&slots[0], &slots[1], 0x0000000000000005UL) != 0x0000000000000005UL || slots[0] != 0x0000000000000001UL || slots[1] != 0x0000000000000005UL) return 2;
    slots[0] = 0xffffffffffffffffUL; slots[1] = 0x0000000000000007UL;
    if (bench_pointer_alias(&slots[0], &slots[0], 0x0000000000000000UL) != 0xffffffffffffffffUL || slots[0] != 0x0000000000000000UL || slots[1] != 0x0000000000000007UL) return 3;
    slots[0] = 0x8000000000000000UL; slots[1] = 0x0000000000000011UL;
    if (bench_pointer_alias(&slots[0], &slots[1], 0xffffffffffffffffUL) != 0xffffffffffffffffUL || slots[0] != 0x8000000000000000UL || slots[1] != 0xffffffffffffffffUL) return 4;
    slots[0] = 0x0000000000000063UL; slots[1] = 0x0000000000000007UL;
    if (bench_pointer_alias(&slots[0], &slots[0], 0x000000000000002aUL) != 0x0000000000000073UL || slots[0] != 0x000000000000002aUL || slots[1] != 0x0000000000000007UL) return 5;
    slots[0] = 0x0000000000000005UL; slots[1] = 0x0000000000000008UL;
    if (bench_pointer_alias(&slots[0], &slots[1], 0x0000000000000002UL) != 0x0000000000000002UL || slots[0] != 0x0000000000000005UL || slots[1] != 0x0000000000000002UL) return 6;
    slots[0] = 0x0000000000123456UL; slots[1] = 0x0000000000987654UL;
    if (bench_pointer_alias(&slots[0], &slots[0], 0x000000000000aabbUL) != 0x00000000001349a8UL || slots[0] != 0x000000000000aabbUL || slots[1] != 0x0000000000987654UL) return 7;
    return 0;
}
