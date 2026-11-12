#include "memtkx/core/base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Status                                                            */
/* ------------------------------------------------------------------ */

const char *mtkx_status_string(mtkx_status status) {
  switch (status) {
    case MTKX_OK:
      return "ok";
    case MTKX_ERR_NOMEM:
      return "out of native memory";
    case MTKX_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case MTKX_ERR_NOT_FOUND:
      return "not found";
    case MTKX_ERR_DUPLICATE:
      return "duplicate entry";
    case MTKX_ERR_PARSE:
      return "parse error";
    case MTKX_ERR_IO:
      return "I/O error";
    case MTKX_ERR_SYSTEM:
      return "system error";
    case MTKX_ERR_RANGE:
      return "value out of range";
  }
  return "unknown status";
}

int mtkx_version(void) { return MTKX_VERSION; }

/* ------------------------------------------------------------------ */
/*  Panic                                                             */
/* ------------------------------------------------------------------ */

MTKX_NORETURN static void mtkx__default_panic(const char *message) {
  fprintf(stderr, "MemTKX panic: %s\n", message != NULL ? message : "(null)");
  fflush(stderr);
  abort();
}

static mtkx_panic_handler mtkx__panic_handler = mtkx__default_panic;

mtkx_panic_handler mtkx_set_panic_handler(mtkx_panic_handler handler) {
  mtkx_panic_handler previous = mtkx__panic_handler;
  mtkx__panic_handler = handler != NULL ? handler : mtkx__default_panic;
  return previous;
}

void mtkx_panic(const char *message) {
  mtkx__panic_handler(message);
  /* handlers must not return; enforce termination if one does */
  abort();
}

/* ------------------------------------------------------------------ */
/*  Allocation hooks                                                  */
/* ------------------------------------------------------------------ */

static mtkx_allocator_hooks mtkx__hooks = {malloc, calloc, realloc, free};

void mtkx_set_allocator(const mtkx_allocator_hooks *hooks) {
  mtkx__hooks.malloc_fn = (hooks != NULL && hooks->malloc_fn) ? hooks->malloc_fn : malloc;
  mtkx__hooks.calloc_fn = (hooks != NULL && hooks->calloc_fn) ? hooks->calloc_fn : calloc;
  mtkx__hooks.realloc_fn =
      (hooks != NULL && hooks->realloc_fn) ? hooks->realloc_fn : realloc;
  mtkx__hooks.free_fn = (hooks != NULL && hooks->free_fn) ? hooks->free_fn : free;
}

void *mtkx_malloc(size_t size) { return mtkx__hooks.malloc_fn(size == 0 ? 1 : size); }

void *mtkx_calloc(size_t count, size_t size) {
  if (count == 0 || size == 0) {
    count = 1;
    size = 1;
  }
  return mtkx__hooks.calloc_fn(count, size);
}

void *mtkx_realloc(void *ptr, size_t size) {
  return mtkx__hooks.realloc_fn(ptr, size == 0 ? 1 : size);
}

void mtkx_free(void *ptr) {
  if (ptr != NULL) {
    mtkx__hooks.free_fn(ptr);
  }
}

char *mtkx_strndup(const char *text, size_t length) {
  char *copy = (char *)mtkx_malloc(length + 1);
  if (copy == NULL) {
    return NULL;
  }
  if (length != 0) {
    memcpy(copy, text, length);
  }
  copy[length] = '\0';
  return copy;
}

char *mtkx_strdup(const char *text) {
  if (text == NULL) {
    text = "";
  }
  return mtkx_strndup(text, strlen(text));
}
