#ifndef _CEPHYR_WCHAR_H
#define _CEPHYR_WCHAR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/_wide_types.h>

struct tm;
size_t wcsftime(wchar_t *, size_t, const wchar_t *, const struct tm *);

wint_t btowc(int);
int wctob(wint_t);
int mbsinit(const mbstate_t *);
size_t mbrlen(const char *, size_t, mbstate_t *);
size_t mbrtowc(wchar_t *, const char *, size_t, mbstate_t *);
size_t wcrtomb(char *, wchar_t, mbstate_t *);

typedef const char **__cephyr_mb_source_pointer;
typedef const wchar_t **__cephyr_wc_source_pointer;
size_t mbsrtowcs(wchar_t *, __cephyr_mb_source_pointer, size_t, mbstate_t *);
size_t wcsrtombs(char *, __cephyr_wc_source_pointer, size_t, mbstate_t *);

#endif
