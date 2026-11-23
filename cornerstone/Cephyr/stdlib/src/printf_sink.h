#ifndef CEPHYR_PRINTF_SINK_H
#define CEPHYR_PRINTF_SINK_H

#include <stddef.h>

/* Floating fields contain only C-locale ASCII. Reserve the entire field in
   destination characters before writing runs; each operation returns 0 or -1. */
struct fp_sink {
    void *context;
    int (*reserve)(void *, size_t);
    int (*write)(void *, const char *, size_t);
    int (*repeat)(void *, char, size_t);
};

int __cephyr_printf_float(const struct fp_sink *sink, double value, char conversion,
                          int alternate, int plus, int blank, int left, int zero,
                          int width, int precision);

#endif
