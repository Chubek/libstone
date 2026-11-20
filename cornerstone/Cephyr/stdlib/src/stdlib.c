#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static int digit_value(unsigned char ch)
{
    if (ch >= '0' && ch <= '9')
        return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'z')
        return (int)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'Z')
        return (int)(ch - 'A') + 10;
    return -1;
}

static unsigned long long
parse_magnitude(const char *str, char **end, int base,
                unsigned long long positive_limit,
                unsigned long long negative_limit, int *negative,
                int *overflow)
{
    const char *original = str;
    unsigned long long value = 0;
    unsigned long long limit;
    unsigned long long cutoff;
    unsigned long long cutlim;
    int any = 0;

    *negative = 0;
    *overflow = 0;
    if (base != 0 && (base < 2 || base > 36)) {
        errno = EINVAL;
        if (end != NULL)
            *end = (char *)original;
        return 0;
    }

    while (isspace((unsigned char)*str))
        ++str;
    if (*str == '+' || *str == '-') {
        *negative = *str == '-';
        ++str;
    }

    if ((base == 0 || base == 16) && str[0] == '0' &&
        (str[1] == 'x' || str[1] == 'X') &&
        digit_value((unsigned char)str[2]) >= 0 &&
        digit_value((unsigned char)str[2]) < 16) {
        base = 16;
        str += 2;
    } else if (base == 0) {
        base = *str == '0' ? 8 : 10;
    }

    limit = *negative ? negative_limit : positive_limit;
    cutoff = limit / (unsigned)base;
    cutlim = limit % (unsigned)base;

    for (;;) {
        int digit = digit_value((unsigned char)*str);
        if (digit < 0 || digit >= base)
            break;
        any = 1;
        if (value > cutoff ||
            (value == cutoff && (unsigned)digit > cutlim)) {
            *overflow = 1;
        } else if (!*overflow) {
            value = value * (unsigned)base + (unsigned)digit;
        }
        ++str;
    }

    if (!any) {
        if (end != NULL)
            *end = (char *)original;
        return 0;
    }
    if (end != NULL)
        *end = (char *)str;
    if (*overflow) {
        errno = ERANGE;
        return limit;
    }
    return value;
}

unsigned long long strtoull(const char *str, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long long value =
        parse_magnitude(str, end, base, ULLONG_MAX, ULLONG_MAX, &negative,
                        &overflow);

    if (overflow)
        return ULLONG_MAX;
    return negative ? 0ULL - value : value;
}

unsigned long strtoul(const char *str, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long long value =
        parse_magnitude(str, end, base, ULONG_MAX, ULONG_MAX, &negative,
                        &overflow);

    if (overflow)
        return ULONG_MAX;
    if (negative)
        return (unsigned long)(0UL - (unsigned long)value);
    return (unsigned long)value;
}

long long strtoll(const char *str, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long long value =
        parse_magnitude(str, end, base, (unsigned long long)LLONG_MAX,
                        (unsigned long long)LLONG_MAX + 1ULL, &negative,
                        &overflow);

    if (overflow)
        return negative ? LLONG_MIN : LLONG_MAX;
    if (negative) {
        if (value == (unsigned long long)LLONG_MAX + 1ULL)
            return LLONG_MIN;
        return -(long long)value;
    }
    return (long long)value;
}

long strtol(const char *str, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long long value =
        parse_magnitude(str, end, base, (unsigned long long)LONG_MAX,
                        (unsigned long long)LONG_MAX + 1ULL, &negative,
                        &overflow);

    if (overflow)
        return negative ? LONG_MIN : LONG_MAX;
    if (negative) {
        if (value == (unsigned long long)LONG_MAX + 1ULL)
            return LONG_MIN;
        return -(long)value;
    }
    return (long)value;
}

int atoi(const char *str)
{
    return (int)strtol(str, NULL, 10);
}

long atol(const char *str)
{
    return strtol(str, NULL, 10);
}

long long atoll(const char *str)
{
    return strtoll(str, NULL, 10);
}

int abs(int value)
{
    return value < 0 ? -value : value;
}

long labs(long value)
{
    return value < 0 ? -value : value;
}

long long llabs(long long value)
{
    return value < 0 ? -value : value;
}

div_t div(int numerator, int denominator)
{
    div_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

ldiv_t ldiv(long numerator, long denominator)
{
    ldiv_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

lldiv_t lldiv(long long numerator, long long denominator)
{
    lldiv_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

intmax_t imaxabs(intmax_t value)
{
    return value < 0 ? -value : value;
}

imaxdiv_t imaxdiv(intmax_t numerator, intmax_t denominator)
{
    imaxdiv_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

intmax_t strtoimax(const char *str, char **end, int base)
{
    return (intmax_t)strtoll(str, end, base);
}

uintmax_t strtoumax(const char *str, char **end, int base)
{
    return (uintmax_t)strtoull(str, end, base);
}

static void swap_bytes(unsigned char *lhs, unsigned char *rhs, size_t size)
{
    while (size != 0) {
        unsigned char temporary = *lhs;
        *lhs++ = *rhs;
        *rhs++ = temporary;
        --size;
    }
}

static void sift_down(unsigned char *base, size_t root, size_t count,
                      size_t size, int (*compare)(const void *, const void *))
{
    for (;;) {
        size_t child;
        size_t selected;

        if (root > (count - 1) / 2)
            return;
        child = root * 2 + 1;
        if (child >= count)
            return;
        selected = root;
        if (compare(base + selected * size, base + child * size) < 0)
            selected = child;
        if (child + 1 < count &&
            compare(base + selected * size, base + (child + 1) * size) < 0)
            selected = child + 1;
        if (selected == root)
            return;
        swap_bytes(base + root * size, base + selected * size, size);
        root = selected;
    }
}

void qsort(void *base_pointer, size_t count, size_t size,
           int (*compare)(const void *, const void *))
{
    unsigned char *base = (unsigned char *)base_pointer;
    size_t start;
    size_t end;

    if (count < 2 || size == 0)
        return;
    start = count / 2;
    while (start != 0) {
        --start;
        sift_down(base, start, count, size, compare);
    }
    end = count;
    while (end > 1) {
        --end;
        swap_bytes(base, base + end * size, size);
        sift_down(base, 0, end, size, compare);
    }
}

void *bsearch(const void *key, const void *base_pointer, size_t count,
              size_t size, int (*compare)(const void *, const void *))
{
    const unsigned char *base = (const unsigned char *)base_pointer;
    size_t low = 0;
    size_t high = count;

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const void *element = base + middle * size;
        int order = compare(key, element);
        if (order < 0)
            high = middle;
        else if (order > 0)
            low = middle + 1;
        else
            return (void *)element;
    }
    return NULL;
}

static uint64_t random_state = UINT64_C(1);


int rand(void)
{
    random_state = random_state * UINT64_C(6364136223846793005) +
                   UINT64_C(1442695040888963407);
    return (int)((random_state >> 33) & RAND_MAX);
}

void srand(unsigned seed)
{
    random_state = seed;
}
