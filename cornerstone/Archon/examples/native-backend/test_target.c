/* Built by the cross compiler, not by the backend under test. */
#include <limits.h>

extern long rotate_pair(long n, long a, long b);
extern unsigned long compute_gcd(unsigned long a, unsigned long b);
extern long sum_array(const long *array, unsigned long n);
extern long factorial(long n);
extern long stack_in(long, long, long, long, long, long, long, long, long, long);
extern long stack_out(long, long, long, long, long, long, long, long, long, long);
extern long memory_roundtrip(long *out, long value);
extern long *zero_word_address(void);
extern void store_word(long *out, long value);
extern long defined_words[];
extern long spill_pressure(long input);
extern long checked_add(long a, long b);
extern long wrapping_add(long a, long b);
extern long checked_sub(long a, long b);
extern long checked_div(long a, long b);
extern long wrapping_div(long a, long b);
extern long wrapping_rem(long a, long b);
extern unsigned long masked_shift(unsigned long a, unsigned long b);

long native_words[] = {41};

long native_ten(long a, long b, long c, long d, long e,
                long f, long g, long h, long i, long j)
{
    return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i + 10*j;
}

#ifndef TRAP_CASE
#define TRAP_CASE 0
#endif

#if TRAP_CASE == 0
static unsigned long reference_gcd(unsigned long a, unsigned long b)
{
    while (b) {
        unsigned long r = a % b;
        a = b;
        b = r;
    }
    return a;
}
#endif

int target_main(void)
{
#if TRAP_CASE == 1
    (void)checked_add(LONG_MAX, 1);
#elif TRAP_CASE == 2
    (void)checked_add(LONG_MIN, -1);
#elif TRAP_CASE == 3
    (void)checked_sub(LONG_MIN, 1);
#elif TRAP_CASE == 4
    (void)checked_sub(LONG_MAX, -1);
#elif TRAP_CASE == 5
    (void)checked_div(17, 0);
#elif TRAP_CASE == 6
    (void)checked_div(LONG_MIN, -1);
#elif TRAP_CASE == 0
    long out[2] = {-7, -8};
    const long array[] = {9, -14, 3, 11};
    long *zero = zero_word_address();
    unsigned long seed = 654;
    unsigned i;

    if (wrapping_add(11, -4) != 7)
        return 10;
    if (sum_array(array, 0) != 0 || sum_array(array, 4) != 9)
        return 20;
    for (i = 0; i < 32; ++i) {
        unsigned long a = (seed = seed * 6364136223846793005UL + 1442695040888963407UL);
        unsigned long b = (seed = seed * 6364136223846793005UL + 1442695040888963407UL);
        if (compute_gcd(a, b) != reference_gcd(a, b))
            return 21;
    }
    if (rotate_pair(0, 2, 7) != 207 || rotate_pair(3, 2, 7) != 702 ||
            rotate_pair(8, 2, 7) != 207)
        return 11;
    if (factorial(0) != 1 || factorial(7) != 5040)
        return 12;
    if (stack_in(1, 2, 3, 4, 5, 6, 7, 8, 99, -32) != 131)
        return 13;
    if (stack_out(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 310)
        return 14;
    if (defined_words[0] != 0x123456789abcdef0L || *zero != 0 ||
            memory_roundtrip(out, -12345678901L) != 70 ||
            out[0] != -12345678901L || out[1] != -8)
        return 15;
    store_word(zero, 500);
    if (*zero_word_address() != 500)
        return 16;
    if (spill_pressure(17) != 1072 || spill_pressure(-5) != 368)
        return 17;
    if (checked_add(LONG_MAX, -3) != LONG_MAX - 3 ||
            checked_add(-13, 5) != -8 ||
            checked_sub(LONG_MIN, -4) != LONG_MIN + 4 ||
            checked_sub(7, 15) != -8 ||
            checked_div(-27, 5) != -5 || checked_div(27, -5) != -5)
        return 18;
    if (wrapping_div(63, 0) != -1 || wrapping_div(LONG_MIN, -1) != LONG_MIN ||
            wrapping_rem(-24, 0) != -24 || wrapping_rem(LONG_MIN, -1) != 0 ||
            masked_shift(1, 127) != (1UL << 63))
        return 19;
    return 0;
#else
#error Unknown TRAP_CASE
#endif
#if TRAP_CASE != 0
    /* Distinguish a missing trap from any ordinary success or failure. */
    return 99;
#endif
}
