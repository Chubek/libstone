#ifndef _CEPHYR_STDLIB_H
#define _CEPHYR_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
#define MB_CUR_MAX ((size_t)1)

typedef struct {
    int quot;
    int rem;
} div_t;

typedef struct {
    long quot;
    long rem;
} ldiv_t;

typedef struct {
    long long quot;
    long long rem;
} lldiv_t;

void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);

_Noreturn void abort(void);
_Noreturn void exit(int);
_Noreturn void _Exit(int);
typedef void (*__cephyr_atexit_function)(void);
int atexit(__cephyr_atexit_function);

int atoi(const char *);
long atol(const char *);
long long atoll(const char *);
#if defined(__CEPHYR__)
#ifndef _CEPHYR_END_POINTER_DEFINED
#define _CEPHYR_END_POINTER_DEFINED
typedef char **__cephyr_end_pointer;
#endif
long strtol(const char *, __cephyr_end_pointer, int);
unsigned long strtoul(const char *, __cephyr_end_pointer, int);
long long strtoll(const char *, __cephyr_end_pointer, int);
unsigned long long strtoull(const char *, __cephyr_end_pointer, int);
#else
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
#endif

int abs(int);
long labs(long);
long long llabs(long long);
#if defined(__CEPHYR__) && !defined(__CEPHYR_HAS_STRUCT_VALUE_ABI)
#define __CEPHYR_STRUCT_VALUE_API_UNAVAILABLE 1
#else
div_t div(int, int);
ldiv_t ldiv(long, long);
lldiv_t lldiv(long long, long long);
#endif

typedef int (*__cephyr_comparison_function)(const void *, const void *);
void qsort(void *, size_t, size_t, __cephyr_comparison_function);
void *bsearch(const void *, const void *, size_t, size_t,
              __cephyr_comparison_function);

int rand(void);
void srand(unsigned);

#endif
