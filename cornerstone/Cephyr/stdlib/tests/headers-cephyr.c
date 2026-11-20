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
#ifndef __CEPHYR_MAX_ALIGN_T_UNAVAILABLE
#error "max_align_t must not use an ABI-wrong substitute"
#endif
#ifndef __CEPHYR_ALIGNAS_UNAVAILABLE
#error "alignas must remain gated while the parser rejects it"
#endif

int cephyr_header_exercise(int value)
{
    char text[4];
    bool positive = value > 0;
    memset(text, 0, sizeof(text));
    text[0] = positive ? (char)toupper('a') : (char)tolower('Z');
    assert(text[0] != '\0');
    return putchar(text[0]);
}
