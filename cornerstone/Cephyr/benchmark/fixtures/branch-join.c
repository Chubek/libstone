static unsigned long pick(unsigned long a, unsigned long b, unsigned long flag)
{
    unsigned long v = flag ? a + 0UL : b * 1UL;
    if (v & 1UL)
        return v ^ 0UL;
    return 0UL | v;
}

int main(void)
{
    if (pick(17UL, 88UL, 0UL) != 88UL)
        return 1;
    if (pick(17UL, 88UL, 1UL) != 17UL)
        return 2;
    return 0;
}
