#include "internal/strbuf.h"

#include <stdio.h>
#include <string.h>

#include "internal/common.h"

void mtkx__strbuf_init(mtkx__strbuf *sb) {
  sb->data = NULL;
  sb->length = 0;
  sb->capacity = 0;
  sb->failed = false;
}

void mtkx__strbuf_destroy(mtkx__strbuf *sb) {
  mtkx_free(sb->data);
  mtkx__strbuf_init(sb);
}

static bool mtkx__strbuf_reserve(mtkx__strbuf *sb, size_t extra) {
  if (sb->failed) {
    return false;
  }
  if (extra > SIZE_MAX - sb->length - 1 ||
      MTKX__GROW(sb->data, sb->capacity, sb->length + extra + 1) != MTKX_OK) {
    sb->failed = true;
    return false;
  }
  return true;
}

void mtkx__strbuf_append(mtkx__strbuf *sb, const char *text, size_t length) {
  if (!mtkx__strbuf_reserve(sb, length)) {
    return;
  }
  if (length != 0) {
    memcpy(sb->data + sb->length, text, length);
  }
  sb->length += length;
  sb->data[sb->length] = '\0';
}

void mtkx__strbuf_append_cstr(mtkx__strbuf *sb, const char *text) {
  mtkx__strbuf_append(sb, text, strlen(text));
}

void mtkx__strbuf_append_char(mtkx__strbuf *sb, char c) { mtkx__strbuf_append(sb, &c, 1); }

void mtkx__strbuf_vappendf(mtkx__strbuf *sb, const char *format, va_list args) {
  va_list copy;
  int needed;
  va_copy(copy, args);
  needed = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (needed < 0) {
    sb->failed = true;
    return;
  }
  if (!mtkx__strbuf_reserve(sb, (size_t)needed)) {
    return;
  }
  vsnprintf(sb->data + sb->length, (size_t)needed + 1, format, args);
  sb->length += (size_t)needed;
}

void mtkx__strbuf_appendf(mtkx__strbuf *sb, const char *format, ...) {
  va_list args;
  va_start(args, format);
  mtkx__strbuf_vappendf(sb, format, args);
  va_end(args);
}

char *mtkx__strbuf_finish(mtkx__strbuf *sb) {
  char *out;
  if (sb->failed) {
    mtkx__strbuf_destroy(sb);
    return NULL;
  }
  if (sb->data == NULL) {
    mtkx__strbuf_init(sb);
    return mtkx_strdup("");
  }
  out = sb->data;
  mtkx__strbuf_init(sb);
  return out;
}
