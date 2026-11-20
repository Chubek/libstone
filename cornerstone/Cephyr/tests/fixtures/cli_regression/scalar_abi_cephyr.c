typedef long (*callback_t)(signed char, unsigned short, int, unsigned, long, long, long, long, long, long);
extern long system_variadic(int count, ...);
extern long external_global;

static long step(signed char a, unsigned short b, int c, unsigned d, long e, long f, long g, long h,
                 long i, long j)
{
    return a + (long)b + c + (long)d + e + f + g + h + i + j;
}

callback_t exported_callback = step;

long cephyr_abi(callback_t callback)
{
    long x = callback(-7, 65535, -11, 0xffffffffU, 5, 6, 7, 8, 9, 10);
    long y = exported_callback(-7, 65535, -11, 0xffffffffU, 5, 6, 7, 8, 9, 10);
    return x + y + system_variadic(9, -1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L) + external_global;
}
