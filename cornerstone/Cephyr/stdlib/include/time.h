#ifndef _CEPHYR_TIME_H
#define _CEPHYR_TIME_H

#include <stddef.h>

typedef long clock_t;
typedef long time_t;

#define CLOCKS_PER_SEC ((clock_t)1000000)
#define TIME_UTC 1

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

clock_t clock(void);
#if defined(__CEPHYR__) && !defined(__CEPHYR_HAS_SCALAR_FP)
#define __CEPHYR_DIFFTIME_UNAVAILABLE 1
#else
double difftime(time_t, time_t);
#endif
time_t mktime(struct tm *);
time_t time(time_t *);
int timespec_get(struct timespec *, int);

char *asctime(const struct tm *);
char *ctime(const time_t *);
struct tm *gmtime(const time_t *);
struct tm *localtime(const time_t *);
size_t strftime(char *, size_t, const char *, const struct tm *);

#endif
