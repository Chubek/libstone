#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

static void clocks(void)
{
    struct timespec ts = { 7, 9 };
    assert(timespec_get(&ts, 0) == 0 && ts.tv_sec == 7 && ts.tv_nsec == 9);
    assert(timespec_get(&ts, TIME_UTC + 1) == 0);
    assert(timespec_get(&ts, TIME_UTC) == TIME_UTC);
    assert(ts.tv_sec > 0 && ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000);
    time_t result;
    time_t now = time(&result);
    assert(now == result && now > 0 && time(NULL) > 0);
    assert(CLOCKS_PER_SEC == 1000000L);
    clock_t begin = clock();
    volatile unsigned work = 1;
    for (unsigned i = 0; i < 100000; ++i)
        work = work * 37 + i;
    clock_t end = clock();
    assert(begin >= 0 && end >= begin);
}

static uint64_t difference_bits(time_t a, time_t b)
{
    /* Check all three ABIs without pulling in software floating-point
       comparisons from an external runtime on RV64IM. */
    union {
        double value;
        uint64_t bits;
    } result;
    result.value = difftime(a, b);
    return result.bits;
}

static void differences(void)
{
    assert(difference_bits(1, 1) == 0);
    assert(difference_bits(LONG_MAX, LONG_MAX - 1) == UINT64_C(0x3ff0000000000000));
    assert(difference_bits(LONG_MIN, LONG_MIN + 1) == UINT64_C(0xbff0000000000000));
    assert(difference_bits(LONG_MAX, LONG_MIN) == UINT64_C(0x43f0000000000000));
    assert(difference_bits(LONG_MIN, LONG_MAX) == UINT64_C(0xc3f0000000000000));
    assert(difference_bits(9007199254740993L, 0) == UINT64_C(0x4340000000000000));
    assert(difference_bits(9007199254740995L, 0) == UINT64_C(0x4340000000000002));
}

static void calendar(void)
{
    time_t epoch = 0;
    struct tm *tm = gmtime(&epoch);
    assert(tm != NULL && tm->tm_year == 70 && tm->tm_mday == 1 &&
           tm->tm_mon == 0 && tm->tm_yday == 0 && tm->tm_wday == 4 &&
           tm->tm_hour == 0 && tm->tm_min == 0 && tm->tm_sec == 0 && tm->tm_isdst == 0);
    assert(strcmp(ctime(&epoch), "Thu Jan  1 00:00:00 1970\n") == 0);

    time_t previous = -1;
    tm = localtime(&previous);
    assert(tm != NULL && tm->tm_year == 69 && tm->tm_mon == 11 &&
           tm->tm_mday == 31 && tm->tm_wday == 3 && tm->tm_yday == 364 &&
           tm->tm_hour == 23 && tm->tm_min == 59 && tm->tm_sec == 59 && !tm->tm_isdst);
    assert(strcmp(asctime(tm), "Wed Dec 31 23:59:59 1969\n") == 0);
    assert(mktime(tm) == previous);

    struct tm normalized = {
        .tm_year = 101, .tm_mon = -11, .tm_mday = 30,
        .tm_hour = -1, .tm_min = 60, .tm_sec = 60,
        .tm_yday = -100, .tm_wday = 200, .tm_isdst = 1
    };
    assert(mktime(&normalized) == 951868860L); /* 2000-03-01 00:01:00 UTC */
    assert(normalized.tm_year == 100 && normalized.tm_mon == 2 &&
           normalized.tm_mday == 1 && normalized.tm_hour == 0 &&
           normalized.tm_min == 1 && normalized.tm_sec == 0 &&
           normalized.tm_yday == 60 && normalized.tm_wday == 3 && !normalized.tm_isdst);

    struct tm start = { .tm_year = -300, .tm_mday = 1 };
    time_t timer = mktime(&start);
    assert(timer == -11676096000L); /* Day one of a full 400-year cycle. */
    for (long day = 0; day < 146097; ++day, timer += 86400) {
        tm = gmtime(&timer);
        assert(tm != NULL && mktime(tm) == timer);
    }
    tm = gmtime(&timer);
    assert(tm != NULL && tm->tm_year == 100 && tm->tm_yday == 0);

    time_t extreme = LONG_MIN;
    errno = 0;
    assert(gmtime(&extreme) == NULL && errno == EOVERFLOW);
    extreme = LONG_MAX;
    errno = 0;
    assert(localtime(&extreme) == NULL && errno == EOVERFLOW);
    struct tm limit = { .tm_year = INT_MAX, .tm_mon = 11, .tm_mday = 31,
                        .tm_hour = 23, .tm_min = 59, .tm_sec = 59 };
    timer = mktime(&limit);
    assert(timer != -1 && gmtime(&timer)->tm_year == INT_MAX);
    ++limit.tm_sec;
    errno = 0;
    assert(mktime(&limit) == -1 && errno == EOVERFLOW);

    limit = (struct tm){ .tm_year = INT_MIN, .tm_mday = 1 };
    timer = mktime(&limit);
    assert(timer != -1 && gmtime(&timer)->tm_year == INT_MIN);
    --limit.tm_sec;
    errno = 0;
    assert(mktime(&limit) == -1 && errno == EOVERFLOW);
}

static void formatting(void)
{
    char buf[256];
    struct tm type = { .tm_year = 99, .tm_mday = 2, .tm_hour = 15,
                       .tm_min = 4, .tm_sec = 5, .tm_isdst = -1 };
    assert(mktime(&type) == 915289445L);
    const char *format =
        "%a %A %b %B %c %C %d %D %e %F %g %G %h %H %I %j %m %M"
        "%n%p %r %R %S%t%T %u %U %V %w %W %x %X %y %Y %z %Z %%";
    const char *expected =
        "Sat Saturday Jan January Sat Jan  2 15:04:05 1999 19 02 01/02/99  2"
        " 1999-01-02 98 1998 Jan 15 03 002 01 04"
        "\nPM 03:04:05 PM 15:04 05\t15:04:05 6 00 53 6 00 01/02/99 15:04:05 99"
        " 1999 +0000 UTC %";
    size_t length = strlen(expected);
    assert(strftime(buf, length + 1, format, &type) == length);
    assert(strcmp(buf, expected) == 0);
    buf[length] = '!';
    assert(strftime(buf, length, format, &type) == 0 && buf[length] == '!');
    assert(strftime(NULL, 0, "X", &type) == 0);
    assert(strftime(buf, 1, "", &type) == 0 && buf[0] == 0);

    const char *alt =
        "%Ec|%EC|%Ex|%EX|%Ey|%EY|%Od %Oe %OH %OI %Om %OM %OS %Ou %OU %OV %Ow %OW %Oy";
    assert(strftime(buf, sizeof(buf), alt, &type) != 0);
    assert(strcmp(buf, "Sat Jan  2 15:04:05 1999|19|01/02/99|15:04:05|99|1999|"
                       "02  2 15 03 01 04 05 6 00 53 6 00 99") == 0);

    type = (struct tm){ .tm_year = 97, .tm_mon = 11, .tm_mday = 30 };
    assert(mktime(&type) != -1);
    assert(strftime(buf, sizeof(buf), "%G-%V-%u", &type) == 9);
    assert(strcmp(buf, "1998-01-2") == 0);
    type.tm_isdst = -1;
    assert(strftime(buf, sizeof(buf), "[%z][%Z]", &type) == 4);
    assert(strcmp(buf, "[][]") == 0);

    /* Conversion specifiers may use only their documented fields. */
    struct tm seconds;
    seconds.tm_sec = 60;
    assert(strftime(buf, sizeof(buf), "%S", &seconds) == 2 && strcmp(buf, "60") == 0);

    wchar_t wide[10] = {0};
    const wchar_t wf[] = { 0x263a, ' ', '%', 'O', 'S', ' ', '%', '%', 0 };
    assert(wcsftime(wide, 7, wf, &seconds) == 6);
    assert(wide[0] == 0x263a && wide[1] == ' ' && wide[2] == '6' &&
           wide[3] == '0' && wide[4] == ' ' && wide[5] == '%' && !wide[6]);
    wide[6] = 'X';
    assert(wcsftime(wide, 6, wf, &seconds) == 0 && wide[6] == 'X');
}

int main(void)
{
    clocks();
    differences();
    calendar();
    formatting();
    return 0;
}
