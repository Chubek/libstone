/*
 * Internal growable string buffer.
 */
#ifndef MEMTKX_INTERNAL_STRBUF_H
#define MEMTKX_INTERNAL_STRBUF_H

#include <stdarg.h>
#include <stddef.h>

#include "memtkx/core/base.h"

typedef struct mtkx__strbuf {
  char *data;
  size_t length;
  size_t capacity;
  bool failed; /* sticky: an append ran out of memory */
} mtkx__strbuf;

void mtkx__strbuf_init(mtkx__strbuf *sb);
void mtkx__strbuf_destroy(mtkx__strbuf *sb);
void mtkx__strbuf_append(mtkx__strbuf *sb, const char *text, size_t length);
void mtkx__strbuf_append_cstr(mtkx__strbuf *sb, const char *text);
void mtkx__strbuf_append_char(mtkx__strbuf *sb, char c);
void mtkx__strbuf_appendf(mtkx__strbuf *sb, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void mtkx__strbuf_vappendf(mtkx__strbuf *sb, const char *format, va_list args);

/* Returns the NUL-terminated string (ownership transfers) or NULL if any
 * append failed (the buffer is released either way). */
char *mtkx__strbuf_finish(mtkx__strbuf *sb);

#endif /* MEMTKX_INTERNAL_STRBUF_H */
