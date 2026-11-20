#ifndef _CEPHYR_STRING_H
#define _CEPHYR_STRING_H

#include <stddef.h>

#if defined(__CEPHYR__)
#define __CEPHYR_RESTRICT
#else
#define __CEPHYR_RESTRICT restrict
#endif

void *memcpy(void *__CEPHYR_RESTRICT,
             const void *__CEPHYR_RESTRICT, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);

size_t strlen(const char *);
size_t strnlen(const char *, size_t);
char *strcpy(char *__CEPHYR_RESTRICT, const char *__CEPHYR_RESTRICT);
char *strncpy(char *__CEPHYR_RESTRICT, const char *__CEPHYR_RESTRICT,
              size_t);
char *strcat(char *__CEPHYR_RESTRICT, const char *__CEPHYR_RESTRICT);
char *strncat(char *__CEPHYR_RESTRICT, const char *__CEPHYR_RESTRICT,
              size_t);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
int strcoll(const char *, const char *);
size_t strxfrm(char *__CEPHYR_RESTRICT, const char *__CEPHYR_RESTRICT,
               size_t);
char *strchr(const char *, int);
char *strrchr(const char *, int);
size_t strspn(const char *, const char *);
size_t strcspn(const char *, const char *);
char *strpbrk(const char *, const char *);
char *strstr(const char *, const char *);
char *strtok(char *__CEPHYR_RESTRICT, const char *__CEPHYR_RESTRICT);
char *strerror(int);

#undef __CEPHYR_RESTRICT

#endif
