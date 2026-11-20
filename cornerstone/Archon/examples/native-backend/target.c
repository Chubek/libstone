/* Target program: exercise the same LP64 interface in both directions. */
extern long choose_max(long a, long b);
extern long scale_max(long a, long b);

long native_scale(long value)
{
    return value * 3;
}

int target_main(void)
{
    if (choose_max(17, -8) != 17 || choose_max(-9, 9) != 9)
        return 1;
    if (scale_max(17, -8) != 51 || scale_max(-9, 9) != 27)
        return 2;
    return 0;
}
