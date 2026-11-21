static int promoted(short a, unsigned char b)
{
    return ((a + 0) - (b + 0)) * 1;
}

int main(void)
{
    if (promoted(-127, 255) != -382)
        return 1;
    if (promoted(32767, 0) != 32767)
        return 2;
    return 0;
}
