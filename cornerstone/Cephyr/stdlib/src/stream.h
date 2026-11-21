#ifndef CEPHYR_LIBC_STREAM_H
#define CEPHYR_LIBC_STREAM_H

#include <stdio.h>

enum { CEPHYR_STREAM_BYTE = -1, CEPHYR_STREAM_WIDE = 1 };

/* Select the orientation on first use; set the stream error on mismatch. */
int __cephyr_stdio_orient(FILE *, int);

#endif
