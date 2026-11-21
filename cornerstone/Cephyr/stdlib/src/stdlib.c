#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int digit_value(wint_t ch)
{
    if (ch >= '0' && ch <= '9')
        return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'z')
        return (int)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'Z')
        return (int)(ch - 'A') + 10;
    return -1;
}

static wint_t integer_char(const void *str, int wide, size_t index)
{
    return wide ? (wint_t)((const wchar_t *)str)[index]
                : ((const unsigned char *)str)[index];
}

static int integer_space(wint_t c)
{
    return c <= 0x7f && isspace((int)c);
}

/* The narrow and wide integer grammars are identical in the C locale.
   Index whole elements so a non-ASCII wide value cannot become a digit
   by truncation, and so the original end pointer needs no byte conversion. */
static unsigned long long
parse_magnitude(const void *str, size_t *end, int wide, int base,
                unsigned long long positive_limit,
                unsigned long long negative_limit, int *negative,
                int *overflow)
{
    size_t i = 0;
    unsigned long long value = 0;
    unsigned long long limit;
    unsigned long long cutoff;
    unsigned long long cutlim;
    size_t digits;

    *negative = 0;
    *overflow = 0;
    *end = 0;
    if (base != 0 && (base < 2 || base > 36)) {
        errno = EINVAL;
        return 0;
    }

    while (integer_space(integer_char(str, wide, i)))
        ++i;
    if (integer_char(str, wide, i) == '+' || integer_char(str, wide, i) == '-') {
        *negative = integer_char(str, wide, i) == '-';
        ++i;
    }

    if ((base == 0 || base == 16) && integer_char(str, wide, i) == '0') {
        wint_t prefix = integer_char(str, wide, i + 1);
        if (prefix == 'x' || prefix == 'X') {
            int digit = digit_value(integer_char(str, wide, i + 2));
            if (digit >= 0 && digit < 16) {
                base = 16;
                i += 2;
            }
        }
    }
    if (base == 0)
        base = integer_char(str, wide, i) == '0' ? 8 : 10;

    limit = *negative ? negative_limit : positive_limit;
    cutoff = limit / (unsigned)base;
    cutlim = limit % (unsigned)base;

    digits = i;
    for (;;) {
        int digit = digit_value(integer_char(str, wide, i));
        if (digit < 0 || digit >= base)
            break;
        if (value > cutoff ||
            (value == cutoff && (unsigned)digit > cutlim)) {
            *overflow = 1;
        } else if (!*overflow) {
            value = value * (unsigned)base + (unsigned)digit;
        }
        ++i;
    }

    if (i == digits)
        return 0;
    *end = i;
    if (*overflow) {
        errno = ERANGE;
        return limit;
    }
    return value;
}

static unsigned long long parse_unsigned(const void *str, size_t *end, int wide,
                                         int base, unsigned long long max)
{
    int negative;
    int overflow;
    unsigned long long value =
        parse_magnitude(str, end, wide, base, max, max, &negative, &overflow);

    if (overflow)
        return max;
    return negative ? 0ULL - value : value;
}

static long long parse_signed(const void *str, size_t *end, int wide, int base,
                              long long min, long long max)
{
    int negative;
    int overflow;
    unsigned long long negative_limit = 0ULL - (unsigned long long)min;
    unsigned long long value =
        parse_magnitude(str, end, wide, base, (unsigned long long)max,
                        negative_limit, &negative, &overflow);

    if (overflow)
        return negative ? min : max;
    if (negative)
        return value == negative_limit ? min : -(long long)value;
    return (long long)value;
}

unsigned long long strtoull(const char *str, char **end, int base)
{
    size_t size;
    unsigned long long value = parse_unsigned(str, &size, 0, base, ULLONG_MAX);
    if (end != NULL)
        *end = (char *)str + size;
    return value;
}

unsigned long strtoul(const char *str, char **end, int base)
{
    size_t size;
    unsigned long value =
        (unsigned long)parse_unsigned(str, &size, 0, base, ULONG_MAX);
    if (end != NULL)
        *end = (char *)str + size;
    return value;
}

long long strtoll(const char *str, char **end, int base)
{
    size_t size;
    long long value = parse_signed(str, &size, 0, base, LLONG_MIN, LLONG_MAX);
    if (end != NULL)
        *end = (char *)str + size;
    return value;
}

long strtol(const char *str, char **end, int base)
{
    size_t size;
    long value = (long)parse_signed(str, &size, 0, base, LONG_MIN, LONG_MAX);
    if (end != NULL)
        *end = (char *)str + size;
    return value;
}

unsigned long long wcstoull(const wchar_t *restrict str, wchar_t **restrict end,
                            int base)
{
    size_t size;
    unsigned long long value = parse_unsigned(str, &size, 1, base, ULLONG_MAX);
    if (end != NULL)
        *end = (wchar_t *)str + size;
    return value;
}

unsigned long wcstoul(const wchar_t *restrict str, wchar_t **restrict end, int base)
{
    size_t size;
    unsigned long value =
        (unsigned long)parse_unsigned(str, &size, 1, base, ULONG_MAX);
    if (end != NULL)
        *end = (wchar_t *)str + size;
    return value;
}

long long wcstoll(const wchar_t *restrict str, wchar_t **restrict end, int base)
{
    size_t size;
    long long value = parse_signed(str, &size, 1, base, LLONG_MIN, LLONG_MAX);
    if (end != NULL)
        *end = (wchar_t *)str + size;
    return value;
}

long wcstol(const wchar_t *restrict str, wchar_t **restrict end, int base)
{
    size_t size;
    long value = (long)parse_signed(str, &size, 1, base, LONG_MIN, LONG_MAX);
    if (end != NULL)
        *end = (wchar_t *)str + size;
    return value;
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

intmax_t wcstoimax(const wchar_t *restrict str, wchar_t **restrict end, int base)
{
    size_t size;
    intmax_t value =
        (intmax_t)parse_signed(str, &size, 1, base, INTMAX_MIN, INTMAX_MAX);
    if (end != NULL)
        *end = (wchar_t *)str + size;
    return value;
}

uintmax_t wcstoumax(const wchar_t *restrict str, wchar_t **restrict end, int base)
{
    size_t size;
    uintmax_t value = (uintmax_t)parse_unsigned(str, &size, 1, base, UINTMAX_MAX);
    if (end != NULL)
        *end = (wchar_t *)str + size;
    return value;
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

char *setlocale(int category, const char *locale)
{
    static char c_locale[] = "C";
    /* The native environment and the startup locale both use ASCII C. */
    if (category < LC_ALL || category > LC_TIME)
        return NULL;
    if (locale == NULL || *locale == '\0' || strcmp(locale, "C") == 0 ||
        strcmp(locale, "POSIX") == 0)
        return c_locale;
    return NULL;
}

struct lconv *localeconv(void)
{
    static char dot[] = ".";
    static char empty[] = "";
    static struct lconv conventions = {
        .decimal_point = dot,
        .thousands_sep = empty,
        .grouping = empty,
        .mon_decimal_point = empty,
        .mon_thousands_sep = empty,
        .mon_grouping = empty,
        .positive_sign = empty,
        .negative_sign = empty,
        .currency_symbol = empty,
        .frac_digits = CHAR_MAX,
        .p_cs_precedes = CHAR_MAX,
        .n_cs_precedes = CHAR_MAX,
        .p_sep_by_space = CHAR_MAX,
        .n_sep_by_space = CHAR_MAX,
        .p_sign_posn = CHAR_MAX,
        .n_sign_posn = CHAR_MAX,
        .int_curr_symbol = empty,
        .int_frac_digits = CHAR_MAX,
        .int_p_cs_precedes = CHAR_MAX,
        .int_n_cs_precedes = CHAR_MAX,
        .int_p_sep_by_space = CHAR_MAX,
        .int_n_sep_by_space = CHAR_MAX,
        .int_p_sign_posn = CHAR_MAX,
        .int_n_sign_posn = CHAR_MAX,
    };
    return &conventions;
}

static int decode_character(wchar_t *wide, const char *str, size_t size)
{
    unsigned char byte;
    if (str == NULL)
        return 0;
    if (size == 0 || (byte = (unsigned char)*str) > 0x7f) {
        errno = EILSEQ;
        return -1;
    }
    if (wide != NULL)
        *wide = (wchar_t)byte;
    return byte == '\0' ? 0 : 1;
}

static int encode_character(char *str, wchar_t wide)
{
    if ((unsigned int)wide > 0x7f) {
        errno = EILSEQ;
        return -1;
    }
    *str = (char)wide;
    return 1;
}

int mblen(const char *str, size_t size)
{
    return decode_character(NULL, str, size);
}

int mbtowc(wchar_t *wide, const char *str, size_t size)
{
    return decode_character(wide, str, size);
}

int wctomb(char *str, wchar_t wide)
{
    return str == NULL ? 0 : encode_character(str, wide);
}

size_t mbstowcs(wchar_t *wide, const char *str, size_t size)
{
    size_t i;
    for (i = 0; wide == NULL || i < size; ++i) {
        int result = decode_character(wide == NULL ? NULL : wide + i, str + i, 1);
        if (result < 0)
            return (size_t)-1;
        if (result == 0)
            break;
    }
    return i;
}

size_t wcstombs(char *str, const wchar_t *wide, size_t size)
{
    size_t i;
    for (i = 0; str == NULL || i < size; ++i) {
        char byte;
        if (encode_character(&byte, wide[i]) < 0)
            return (size_t)-1;
        if (str != NULL)
            str[i] = byte;
        if (byte == '\0')
            break;
    }
    return i;
}
