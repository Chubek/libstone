#ifndef _CEPHYR_UCHAR_H
#define _CEPHYR_UCHAR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/_wide_types.h>

typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;

size_t mbrtoc16(char16_t *, const char *, size_t, mbstate_t *);
size_t c16rtomb(char *, char16_t, mbstate_t *);
size_t mbrtoc32(char32_t *, const char *, size_t, mbstate_t *);
size_t c32rtomb(char *, char32_t, mbstate_t *);

#endif
