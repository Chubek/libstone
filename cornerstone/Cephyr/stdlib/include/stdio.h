#ifndef _CEPHYR_STDIO_H
#define _CEPHYR_STDIO_H

#include <stddef.h>

#define EOF (-1)
#define BUFSIZ 1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct __cephyr_FILE FILE;
typedef long fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define stdin stdin
#define stdout stdout
#define stderr stderr

int fflush(FILE *);
void clearerr(FILE *);
int feof(FILE *);
int ferror(FILE *);
int fgetc(FILE *);
int getc(FILE *);
int getchar(void);
int fputc(int, FILE *);
int putc(int, FILE *);
int putchar(int);
char *fgets(char *, int, FILE *);
int fputs(const char *, FILE *);
int puts(const char *);
void perror(const char *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);

#if !defined(__CEPHYR__) || defined(__CEPHYR_HAS_VARIADIC_CALLS)
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
#else
#define __CEPHYR_VARIADIC_IO_UNAVAILABLE 1
#endif

#if !defined(__CEPHYR__)
#include <stdarg.h>
int vprintf(const char *, va_list);
int vfprintf(FILE *, const char *, va_list);
int vsprintf(char *, const char *, va_list);
int vsnprintf(char *, size_t, const char *, va_list);
#endif

#endif
