#ifndef _CEPHYR_WCHAR_H
#define _CEPHYR_WCHAR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/_wide_types.h>

struct __cephyr_FILE;
wint_t fgetwc(struct __cephyr_FILE *);
wchar_t *fgetws(wchar_t *restrict, int, struct __cephyr_FILE *restrict);
wint_t fputwc(wchar_t, struct __cephyr_FILE *);
int fputws(const wchar_t *restrict, struct __cephyr_FILE *restrict);
int fwide(struct __cephyr_FILE *, int);
wint_t getwc(struct __cephyr_FILE *);
wint_t getwchar(void);
wint_t putwc(wchar_t, struct __cephyr_FILE *);
wint_t putwchar(wchar_t);
wint_t ungetwc(wint_t, struct __cephyr_FILE *);

wchar_t *wcscpy(wchar_t *restrict, const wchar_t *restrict);
wchar_t *wcsncpy(wchar_t *restrict, const wchar_t *restrict, size_t);
wchar_t *wmemcpy(wchar_t *restrict, const wchar_t *restrict, size_t);
wchar_t *wmemmove(wchar_t *, const wchar_t *, size_t);
wchar_t *wcscat(wchar_t *restrict, const wchar_t *restrict);
wchar_t *wcsncat(wchar_t *restrict, const wchar_t *restrict, size_t);
int wcscmp(const wchar_t *, const wchar_t *);
int wcscoll(const wchar_t *, const wchar_t *);
int wcsncmp(const wchar_t *, const wchar_t *, size_t);
size_t wcsxfrm(wchar_t *restrict, const wchar_t *restrict, size_t);
int wmemcmp(const wchar_t *, const wchar_t *, size_t);
wchar_t *wcschr(const wchar_t *, wchar_t);
size_t wcscspn(const wchar_t *, const wchar_t *);
wchar_t *wcspbrk(const wchar_t *, const wchar_t *);
wchar_t *wcsrchr(const wchar_t *, wchar_t);
size_t wcsspn(const wchar_t *, const wchar_t *);
wchar_t *wcsstr(const wchar_t *, const wchar_t *);
typedef wchar_t **__cephyr_wc_pointer_pointer;
wchar_t *wcstok(wchar_t *restrict, const wchar_t *restrict,
                __cephyr_wc_pointer_pointer restrict);
wchar_t *wmemchr(const wchar_t *, wchar_t, size_t);
size_t wcslen(const wchar_t *);
wchar_t *wmemset(wchar_t *, wchar_t, size_t);

long wcstol(const wchar_t *restrict, __cephyr_wc_pointer_pointer restrict, int);
long long wcstoll(const wchar_t *restrict, __cephyr_wc_pointer_pointer restrict, int);
unsigned long wcstoul(const wchar_t *restrict, __cephyr_wc_pointer_pointer restrict, int);
unsigned long long wcstoull(const wchar_t *restrict, __cephyr_wc_pointer_pointer restrict, int);

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
