static unsigned long identity(unsigned long x)
{
    return (((x + 0UL) - 0UL) ^ 0UL) * 1UL;
}

int main(void)
{
    if (identity(0UL) != 0UL)
        return 1;
    if (identity(0xfedcba9876543210UL) != 0xfedcba9876543210UL)
        return 2;
    return 0;
}
