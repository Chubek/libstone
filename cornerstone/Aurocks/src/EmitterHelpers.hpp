#pragma once

#include <string_view>

namespace aurocks::detail {

// Legacy v2 helpers, with ordinary heap ownership (no external string pool).
inline constexpr std::string_view LegacyRuntimeHelpers = R"AUROCKS(
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define AUROCKS_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#  define AUROCKS_THREAD_LOCAL __thread
#elif defined(_MSC_VER)
#  define AUROCKS_THREAD_LOCAL __declspec(thread)
#else
#  define AUROCKS_THREAD_LOCAL
#endif

void *aurocks_alloc(size_t size) {
    return calloc(1, size ? size : 1);
}
void *aurocks_calloc(size_t count, size_t size) {
    if (count && size > (size_t)-1 / count) return NULL;
    return calloc(count ? count : 1, size ? size : 1);
}
void *aurocks_realloc(void *ptr, size_t size) {
    if (!size) { free(ptr); return NULL; }
    return realloc(ptr, size);
}
void aurocks_free(void *ptr) { free(ptr); }

char *aurocks_strndup(const char *s, size_t n) {
    size_t len = 0;
    char *copy;
    if (!s) return NULL;
    while (len < n && s[len]) ++len;
    copy = (char *)malloc(len + 1);
    if (copy) { memcpy(copy, s, len); copy[len] = '\0'; }
    return copy;
}
char *aurocks_strdup(const char *s) {
    return s ? aurocks_strndup(s, strlen(s)) : NULL;
}
void *aurocks_memdup(const void *src, size_t n) {
    void *copy;
    if (n && !src) return NULL;
    copy = malloc(n ? n : 1);
    if (copy && n) memcpy(copy, src, n);
    return copy;
}
size_t aurocks_strlen(const char *s) { return s ? strlen(s) : 0; }
int aurocks_strcmp(const char *a, const char *b) {
    return a == b ? 0 : !a ? -1 : !b ? 1 : strcmp(a, b);
}
int aurocks_strncmp(const char *a, const char *b, size_t n) {
    return !n || a == b ? 0 : !a ? -1 : !b ? 1 : strncmp(a, b, n);
}
int aurocks_strcasecmp(const char *a, const char *b) {
    unsigned char x, y;
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    do {
        x = (unsigned char)*a++;
        y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
        if (x != y) return (int)x - (int)y;
    } while (x);
    return 0;
}
const char *aurocks_strchr(const char *s, int c) { return s ? strchr(s, c) : NULL; }
const char *aurocks_strrchr(const char *s, int c) { return s ? strrchr(s, c) : NULL; }
const char *aurocks_strstr(const char *h, const char *n) { return h && n ? strstr(h, n) : NULL; }
size_t aurocks_strlcpy(char *dst, const char *src, size_t size) {
    const size_t length = aurocks_strlen(src);
    if (dst && size) {
        const size_t n = length < size - 1 ? length : size - 1;
        if (n) memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return length;
}

#define AUROCKS_WRAP_BASE(TYPE, NAME) \
    TYPE aurocks_##NAME(const char *s, char **end, int base) { return s ? NAME(s, end, base) : 0; }
#define AUROCKS_WRAP_END(TYPE, NAME) \
    TYPE aurocks_##NAME(const char *s, char **end) { return s ? NAME(s, end) : 0; }
#define AUROCKS_WRAP_TEXT(TYPE, NAME) \
    TYPE aurocks_##NAME(const char *s) { return s ? NAME(s) : 0; }
AUROCKS_WRAP_BASE(long, strtol)
AUROCKS_WRAP_BASE(unsigned long, strtoul)
AUROCKS_WRAP_BASE(long long, strtoll)
AUROCKS_WRAP_BASE(unsigned long long, strtoull)
AUROCKS_WRAP_END(double, strtod)
float aurocks_strtof(const char *s, char **end) { return (float)aurocks_strtod(s, end); }
AUROCKS_WRAP_END(long double, strtold)
AUROCKS_WRAP_TEXT(int, atoi)
AUROCKS_WRAP_TEXT(long, atol)
AUROCKS_WRAP_TEXT(long long, atoll)
AUROCKS_WRAP_TEXT(double, atof)
#undef AUROCKS_WRAP_BASE
#undef AUROCKS_WRAP_END
#undef AUROCKS_WRAP_TEXT

int aurocks_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    return vsnprintf(buf, size, fmt, ap);
}
int aurocks_snprintf(char *buf, size_t size, const char *fmt, ...) {
    int length;
    va_list ap;
    va_start(ap, fmt);
    length = aurocks_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return length;
}
char *aurocks_vasprintf(const char *fmt, va_list ap) {
    va_list copy;
    int length;
    char *buf;
    va_copy(copy, ap);
    length = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (length < 0) return NULL;
    buf = (char *)malloc((size_t)length + 1);
    if (buf && vsnprintf(buf, (size_t)length + 1, fmt, ap) != length) {
        free(buf);
        return NULL;
    }
    return buf;
}
char *aurocks_asprintf(const char *fmt, ...) {
    va_list ap;
    char *buf;
    va_start(ap, fmt);
    buf = aurocks_vasprintf(fmt, ap);
    va_end(ap);
    return buf;
}

void *aurocks_box_double(double value) { return aurocks_memdup(&value, sizeof value); }
double aurocks_unbox_double(const void *box) { return box ? *(const double *)box : 0.0; }
void *aurocks_box_long(long value) { return aurocks_memdup(&value, sizeof value); }
long aurocks_unbox_long(const void *box) { return box ? *(const long *)box : 0; }
void *aurocks_box_str(const char *text) { return aurocks_strdup(text); }

enum { AUROCKS_RUNTIME_FUNCTION_COUNT = 35 };
const char *const aurocks_runtime_functions[AUROCKS_RUNTIME_FUNCTION_COUNT] = {
    "aurocks_alloc", "aurocks_calloc", "aurocks_realloc", "aurocks_free",
    "aurocks_strdup", "aurocks_strndup", "aurocks_memdup", "aurocks_strlen",
    "aurocks_strcmp", "aurocks_strncmp", "aurocks_strcasecmp", "aurocks_strchr",
    "aurocks_strrchr", "aurocks_strstr", "aurocks_strlcpy", "aurocks_strtol",
    "aurocks_strtoul", "aurocks_strtoll", "aurocks_strtoull", "aurocks_strtod",
    "aurocks_strtof", "aurocks_strtold", "aurocks_atoi", "aurocks_atol",
    "aurocks_atoll", "aurocks_atof", "aurocks_snprintf", "aurocks_vsnprintf",
    "aurocks_asprintf", "aurocks_vasprintf", "aurocks_box_double",
    "aurocks_unbox_double", "aurocks_box_long", "aurocks_unbox_long", "aurocks_box_str",
};
)AUROCKS";

} // namespace aurocks::detail
