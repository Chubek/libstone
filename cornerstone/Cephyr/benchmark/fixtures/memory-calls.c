static unsigned long state[3] = {4UL, 9UL, 15UL};
static unsigned long calls;

static unsigned long bump(unsigned long *p, unsigned long add)
{
    ++calls;
    *p = (*p + add) * 1UL;
    return *p ^ 0UL;
}

int main(void)
{
    unsigned long first = bump(&state[1], 7UL) + 0UL;
    unsigned long second = bump(&state[2], 10UL) * 1UL;
    if (first != 16UL || second != 25UL || calls != 2UL)
        return 1;
    if (state[0] != 4UL || state[1] != first || state[2] != second)
        return 2;
    return 0;
}
