#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <wchar.h>

/* Linux LP64 uses native 64-bit seconds and nanoseconds for both clocks. */
#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_PROCESS_CPUTIME_ID 2
#define SECONDS_PER_DAY 86400L

extern int __cephyr_clock_gettime(int, struct timespec *);

int timespec_get(struct timespec *ts, int base)
{
    if (base != TIME_UTC || __cephyr_clock_gettime(LINUX_CLOCK_REALTIME, ts) < 0)
        return 0;
    return base;
}

time_t time(time_t *timer)
{
    struct timespec ts;
    time_t seconds = timespec_get(&ts, TIME_UTC) == TIME_UTC ? ts.tv_sec : -1;
    if (timer != NULL)
        *timer = seconds;
    return seconds;
}

clock_t clock(void)
{
    struct timespec ts;
    if (__cephyr_clock_gettime(LINUX_CLOCK_PROCESS_CPUTIME_ID, &ts) < 0)
        return -1;
    long fraction = ts.tv_nsec / 1000;
    if (ts.tv_sec < 0 || ts.tv_sec > (LONG_MAX - fraction) / CLOCKS_PER_SEC) {
        errno = EOVERFLOW;
        return -1;
    }
    return ts.tv_sec * CLOCKS_PER_SEC + fraction;
}

double difftime(time_t time1, time_t time0)
{
    int negative = time1 < time0;
    /* Subtract exactly even when the operands are adjacent near LONG_MAX,
       or when their difference does not fit in a signed time_t. */
    uint64_t difference = negative ? (uint64_t)time0 - (uint64_t)time1
                                   : (uint64_t)time1 - (uint64_t)time0;
#if defined(__riscv) && defined(__riscv_float_abi_soft)
    /* RV64IM has no integer-to-double instruction and the runtime has no
       compiler-rt dependency. Round the exact magnitude to binary64. */
    union {
        uint64_t bits;
        double value;
    } result;
    result.bits = 0;
    if (difference != 0) {
        unsigned exponent = 0;
        for (uint64_t v = difference; v > 1; v >>= 1)
            ++exponent;
        uint64_t significand;
        if (exponent > 52) {
            unsigned shift = exponent - 52;
            uint64_t half = UINT64_C(1) << (shift - 1);
            uint64_t tail = difference & (2 * half - 1);
            significand = difference >> shift;
            if (tail > half || (tail == half && (significand & 1)))
                ++significand;
        } else {
            significand = difference << (52 - exponent);
        }
        /* Adding the implicit high bit also carries any rounding overflow
           into the exponent. */
        result.bits = ((UINT64_C(1022) + exponent) << 52) + significand;
        if (negative)
            result.bits |= UINT64_C(1) << 63;
    }
    return result.value;
#else
    return negative ? -(double)difference : (double)difference;
#endif
}

static long floor_div(long value, long divisor)
{
    return value / divisor - (value % divisor < 0);
}

static long floor_mod(long value, long divisor)
{
    long remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

static int year_length(long year)
{
    return 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

/* Gregorian eras begin in March, putting the intercalary day at the
   end of the year. 719468 days separate March 1, year 0, and the epoch. */
static long days_from_date(long year, int month, int day)
{
    year -= month <= 2;
    long era = floor_div(year, 400);
    long era_year = year - era * 400;
    int march_month = month + (month > 2 ? -3 : 9);
    long ordinal = (153 * march_month + 2) / 5 + day - 1;
    long era_day = era_year * 365 + era_year / 4 - era_year / 100 + ordinal;
    return era * 146097 + era_day - 719468;
}

static int breakdown(time_t timer, struct tm *out)
{
    long days = floor_div(timer, SECONDS_PER_DAY);
    long seconds = floor_mod(timer, SECONDS_PER_DAY);
    long march_days = days + 719468;
    long era = floor_div(march_days, 146097);
    long era_day = march_days - era * 146097;
    long era_year = (era_day - era_day / 1460 + era_day / 36524 -
                     era_day / 146096) / 365;
    long year = era * 400 + era_year;
    long ordinal = era_day - (era_year * 365 + era_year / 4 - era_year / 100);
    int march_month = (int)((5 * ordinal + 2) / 153);
    int month = march_month + (march_month < 10 ? 3 : -9);
    year += month <= 2;
    if (year - 1900 < INT_MIN || year - 1900 > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    out->tm_sec = (int)(seconds % 60);
    out->tm_min = (int)(seconds / 60 % 60);
    out->tm_hour = (int)(seconds / 3600);
    out->tm_mday = (int)(ordinal - (153 * march_month + 2) / 5 + 1);
    out->tm_mon = month - 1;
    out->tm_year = (int)(year - 1900);
    out->tm_wday = (int)floor_mod(days + 4, 7); /* 1970-01-01 was Thursday. */
    out->tm_yday = (int)(days - days_from_date(year, 1, 1));
    out->tm_isdst = 0;
    return 0;
}

time_t mktime(struct tm *tm)
{
    /* All source fields are 32-bit ints: even unnormalized, the resulting
       seconds fit into LP64 long. breakdown checks the normalized tm_year. */
    long year = (long)tm->tm_year + 1900 + floor_div(tm->tm_mon, 12);
    int month = (int)floor_mod(tm->tm_mon, 12) + 1;
    long days = days_from_date(year, month, 1) + ((long)tm->tm_mday - 1);
    time_t timer = days * SECONDS_PER_DAY + (long)tm->tm_hour * 3600 +
                   (long)tm->tm_min * 60 + tm->tm_sec;
    return breakdown(timer, tm) < 0 ? -1 : timer;
}

struct tm *gmtime(const time_t *timer)
{
    static struct tm tm;
    return breakdown(*timer, &tm) < 0 ? NULL : &tm;
}

struct tm *localtime(const time_t *timer)
{
    /* This implementation's local timezone is UTC, without DST. */
    return gmtime(timer);
}

static const char *const weekdays[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const months[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

struct time_output {
    unsigned char *bytes;
    wchar_t *wide;
    size_t left;
};

static int emit(struct time_output *output, wchar_t c)
{
    if (output->left == 0)
        return 0;
    if (output->wide != NULL)
        *output->wide++ = c;
    else
        *output->bytes++ = (unsigned char)c;
    --output->left;
    return 1;
}

static int emit_text(struct time_output *output, const char *text, size_t limit)
{
    while (limit-- != 0 && *text != '\0')
        if (!emit(output, *text++))
            return 0;
    return 1;
}

static int emit_number(struct time_output *output, long number,
                       unsigned width, char pad)
{
    char digits[sizeof(long) * 3];
    unsigned length = 0;
    unsigned long magnitude = (unsigned long)number;
    int negative = number < 0;
    if (negative) {
        magnitude = 0 - magnitude;
        if (width != 0)
            --width;
    }
    do {
        digits[length++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (negative && !emit(output, '-'))
        return 0;
    for (unsigned i = length; i < width; ++i)
        if (!emit(output, pad))
            return 0;
    while (length != 0)
        if (!emit(output, digits[--length]))
            return 0;
    return 1;
}

static int format_time(struct time_output *, const char *, const struct tm *);

static int conversion(struct time_output *output, char type, const struct tm *tm)
{
    long number;
    unsigned width = 2;
    char pad = '0';
    switch (type) {
    case 'a': case 'A':
        return (unsigned)tm->tm_wday < 7 &&
               emit_text(output, weekdays[tm->tm_wday], type == 'a' ? 3 : SIZE_MAX);
    case 'b': case 'B': case 'h':
        return (unsigned)tm->tm_mon < 12 &&
               emit_text(output, months[tm->tm_mon], type == 'B' ? SIZE_MAX : 3);
    case 'c': return format_time(output, "%a %b %e %T %Y", tm);
    case 'C': number = ((long)tm->tm_year + 1900) / 100; break;
    case 'D': case 'x': return format_time(output, "%m/%d/%y", tm);
    case 'F': return format_time(output, "%Y-%m-%d", tm);
    case 'e': pad = ' '; /* fall through */
    case 'd': number = tm->tm_mday; break;
    case 'G': case 'g': case 'V': {
        /* The Thursday of the given ISO week determines its week-year. */
        long year = (long)tm->tm_year + 1900;
        long thursday = (long)tm->tm_yday + 3 - floor_mod((long)tm->tm_wday + 6, 7);
        if (thursday < 0)
            thursday += year_length(--year);
        else if (thursday >= year_length(year)) {
            thursday -= year_length(year);
            ++year;
        }
        if (type == 'V')
            number = thursday / 7 + 1;
        else if (type == 'g')
            number = floor_mod(year, 100);
        else {
            number = year;
            width = 4;
        }
        break;
    }
    case 'H': number = tm->tm_hour; break;
    case 'I': number = floor_mod((long)tm->tm_hour - 1, 12) + 1; break;
    case 'j': number = (long)tm->tm_yday + 1; width = 3; break;
    case 'm': number = (long)tm->tm_mon + 1; break;
    case 'M': number = tm->tm_min; break;
    case 'n': return emit(output, '\n');
    case 'p': return emit_text(output, tm->tm_hour < 12 ? "AM" : "PM", 2);
    case 'r': return format_time(output, "%I:%M:%S %p", tm);
    case 'R': return format_time(output, "%H:%M", tm);
    case 'S': number = tm->tm_sec; break;
    case 't': return emit(output, '\t');
    case 'T': case 'X': return format_time(output, "%H:%M:%S", tm);
    case 'u': number = tm->tm_wday ? tm->tm_wday : 7; width = 1; break;
    case 'U': number = ((long)tm->tm_yday + 7 - tm->tm_wday) / 7; break;
    case 'w': number = tm->tm_wday; width = 1; break;
    case 'W': number = ((long)tm->tm_yday + 7 - floor_mod((long)tm->tm_wday + 6, 7)) / 7; break;
    case 'y': number = floor_mod((long)tm->tm_year + 1900, 100); break;
    case 'Y': number = (long)tm->tm_year + 1900; width = 4; break;
    case 'z': return tm->tm_isdst < 0 || emit_text(output, "+0000", 5);
    case 'Z': return tm->tm_isdst < 0 || emit_text(output, "UTC", 3);
    case '%': return emit(output, '%');
    default: return 0;
    }
    return emit_number(output, number, width, pad);
}

static int format_time(struct time_output *output, const char *format, const struct tm *tm)
{
    while (*format != '\0') {
        unsigned char type = (unsigned char)*format++;
        if (type == '%') {
            /* The C locale has no alternative representations. */
            if (*format == 'E' || *format == 'O')
                ++format;
            if (*format == '\0' || !conversion(output, *format++, tm))
                return 0;
        } else if (!emit(output, type)) {
            return 0;
        }
    }
    return 1;
}

size_t strftime(char *s, size_t maxsize, const char *format, const struct tm *tm)
{
    struct time_output output = { (unsigned char *)s, NULL, maxsize };
    return format_time(&output, format, tm) && emit(&output, '\0')
           ? maxsize - output.left - 1 : 0;
}

size_t wcsftime(wchar_t *s, size_t maxsize, const wchar_t *format, const struct tm *tm)
{
    struct time_output output = { NULL, s, maxsize };
    while (*format != 0) {
        wchar_t type = *format++;
        if (type == '%') {
            if (*format == 'E' || *format == 'O')
                ++format;
            type = *format;
            if (type == 0 || (unsigned int)type > 0x7f ||
                !conversion(&output, (char)type, tm))
                return 0;
            ++format;
        } else if (!emit(&output, type)) {
            return 0;
        }
    }
    return emit(&output, 0) ? maxsize - output.left - 1 : 0;
}

char *asctime(const struct tm *tm)
{
    static char buffer[26];
    long year = (long)tm->tm_year + 1900;
    if (year < 1000 || year > 9999 || (unsigned)tm->tm_wday > 6 ||
        (unsigned)tm->tm_mon > 11 || tm->tm_mday < 1 || tm->tm_mday > 31 ||
        (unsigned)tm->tm_hour > 23 || (unsigned)tm->tm_min > 59 ||
        (unsigned)tm->tm_sec > 60) {
        errno = EOVERFLOW;
        return NULL;
    }
    return strftime(buffer, sizeof(buffer), "%c\n", tm) != 0 ? buffer : NULL;
}

char *ctime(const time_t *timer)
{
    struct tm tm;
    return breakdown(*timer, &tm) < 0 ? NULL : asctime(&tm);
}
