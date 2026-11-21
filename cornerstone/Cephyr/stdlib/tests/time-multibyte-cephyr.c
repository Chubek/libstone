#include <setjmp.h>
#include <time.h>
#include <uchar.h>
#include <wchar.h>

#if !defined(__CEPHYR_NONLOCAL_JUMPS_UNAVAILABLE)
#error "setjmp must be gated pending returns-twice support"
#endif
#if !defined(__CEPHYR_HAS_SCALAR_FP) && !defined(__CEPHYR_DIFFTIME_UNAVAILABLE)
#error "difftime must be gated without a supported native floating-point ABI"
#endif

int main(void)
{
    if (time(NULL) <= 0)
        return 1;
#ifdef __CEPHYR_HAS_SCALAR_FP
    if (difftime(8, 11) != -3.0)
        return 7;
#endif

    time_t epoch = 0;
    struct tm *date = gmtime(&epoch);
    if (date == NULL || date->tm_year != 70 || date->tm_mon != 0 || date->tm_mday != 1)
        return 2;
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC ||
        now.tv_sec <= 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L)
        return 8;
    char output[5];
    if (strftime(output, sizeof(output), "%Y", date) != 4 || output[0] != '1')
        return 3;

    mbstate_t state = 0;
    char16_t c16;
    char32_t c32;
    wchar_t wc;
    if (mbrtoc16(&c16, "x", 1, &state) != 1 || c16 != 'x')
        return 4;
    if (mbrtoc32(&c32, "y", 1, NULL) != 1 || c32 != 'y')
        return 5;
    if (mbrtowc(&wc, "a", 1, &state) != 1 || wc != 'a' || !mbsinit(&state))
        return 6;
    return 0;
}
