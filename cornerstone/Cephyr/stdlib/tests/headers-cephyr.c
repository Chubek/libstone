#define __CEPHYR__ 1

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

#if !defined(__CEPHYR_HAS_VARIADIC_CALLS) && \
    !defined(__CEPHYR_VARIADIC_IO_UNAVAILABLE)
#error "variadic I/O must remain gated"
#endif
#if __alignas_is_defined != 1 || __alignof_is_defined != 1
#error "stdalign.h must supply the C11 feature indicators"
#endif

_Static_assert(alignof(max_align_t) == 16, "maximum fundamental alignment");
_Static_assert(sizeof(max_align_t) % alignof(max_align_t) == 0,
               "max_align_t array stride retains its alignment");

/* Check both preprocessing use and the C types after integer promotion. */
#if CHAR_BIT != 8 || MB_LEN_MAX != 1 || UINT64_C(0xffffffffffffffff) != UINTMAX_MAX
#error "unexpected bundled LP64 limits"
#endif
_Static_assert(_Generic(UCHAR_MAX, int: 1, default: 0), "UCHAR_MAX is promoted");
_Static_assert(_Generic(UINT8_C(255), int: 1, default: 0), "UINT8_C is promoted");
_Static_assert(_Generic(UINT16_C(65535), int: 1, default: 0), "UINT16_C is promoted");
_Static_assert(_Generic(UINT32_C(4294967295), unsigned int: 1, default: 0),
               "UINT32_C preserves the unsigned int type");
_Static_assert(_Generic(UINT64_C(0xffffffffffffffff), unsigned long: 1, default: 0),
               "UINT64_C preserves the unsigned long type");

int cephyr_header_exercise(int value)
{
    char text[4];
    bool positive = value > 0;
    memset(text, 0, sizeof(text));
    text[0] = positive ? (char)toupper('a') : (char)tolower('Z');
    assert(text[0] != '\0');
    return putchar(text[0]);
}
