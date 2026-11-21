static unsigned long total(unsigned long count)
{
    unsigned long result = 0;
    for (unsigned long i = 0; i < count; ++i) {
        result = (result + (i ^ 0UL)) * 1UL;
        if (i & 1UL)
            result = (result - 0UL) + i;
    }
    return result + 0UL;
}

int main(void)
{
    if (total(0UL) != 0UL || total(8UL) != 44UL || total(11UL) != 80UL)
        return 1;
    return 0;
}
