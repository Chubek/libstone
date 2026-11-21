static unsigned long high_bits(unsigned long x, unsigned long y)
{
    return (x * 3UL) ^ (y << 5);
}

int main(void)
{
    if (high_bits(7UL, 4UL) != 149UL)
        return 1;
    if (high_bits(0x5000000000000000UL, 5UL) != 0xf0000000000000a0UL)
        return 2;
    return 0;
}
