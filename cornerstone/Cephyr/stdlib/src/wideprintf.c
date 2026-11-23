#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include "printf_sink.h"
#include "stream.h"

struct wide_sink {
    FILE *stream;
    wchar_t *buffer;
    size_t capacity;
    size_t count;
};

static int fail(int error)
{
    errno = error;
    return -1;
}

/* Check a full field before writing it. In particular, a large width cannot
   make a short-buffer format spend time emitting discarded padding. */
static int reserve(struct wide_sink *sink, size_t count)
{
    if (count > (size_t)INT_MAX - sink->count ||
        (sink->stream == NULL &&
         (sink->capacity == 0 || count >= sink->capacity - sink->count)))
        return fail(EOVERFLOW);
    return 0;
}

static int put(struct wide_sink *sink, wchar_t wc)
{
    if (sink->stream != NULL) {
        if (fputwc(wc, sink->stream) == WEOF)
            return -1;
    } else {
        sink->buffer[sink->count] = wc;
    }
    ++sink->count;
    return 0;
}

static int repeat(struct wide_sink *sink, wchar_t wc, size_t count)
{
    while (count != 0) {
        if (put(sink, wc) < 0)
            return -1;
        --count;
    }
    return 0;
}

enum length { DEFAULT, HH, H, L, LL, J, Z, T, BIG_L };
enum { LEFT = 1, PLUS = 2, SPACE = 4, ALT = 8, ZERO = 16 };

struct field {
    unsigned flags;
    int width;
    int precision;
    enum length length;
};

static int fp_wide_reserve(void *context, size_t count)
{
    return reserve(context, count);
}

static int fp_wide_write(void *context, const char *ascii, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (put(context, ascii[i]) < 0)
            return -1;
    return 0;
}

static int fp_wide_repeat(void *context, char ch, size_t count)
{
    return repeat(context, ch, count);
}

static size_t padding(size_t count, const struct field *field)
{
    return (size_t)field->width > count ? (size_t)field->width - count : 0;
}

static int text(struct wide_sink *sink, const wchar_t *wide, const char *bytes,
                size_t count, const struct field *field)
{
    size_t spaces = padding(count, field);
    if (reserve(sink, count + spaces) < 0 ||
        (!(field->flags & LEFT) && repeat(sink, L' ', spaces) < 0))
        return -1;
    for (size_t i = 0; i < count; ++i) {
        /* Narrow text has already been validated in the ASCII C locale. */
        if (put(sink, wide != NULL ? wide[i] : (unsigned char)bytes[i]) < 0)
            return -1;
    }
    return field->flags & LEFT ? repeat(sink, L' ', spaces) : 0;
}

static int string(struct wide_sink *sink, const wchar_t *wide, const char *bytes,
                  const struct field *field)
{
    size_t count = 0;
    size_t limit = field->precision < 0 ? (size_t)INT_MAX + 1
                                       : (size_t)field->precision;
    if (wide != NULL) {
        while (count < limit && wide[count] != L'\0')
            ++count;
    } else {
        mbstate_t state = 0;
        /* Cephyr's only locale consumes one byte per wide character. Do
           not inspect the next byte once the wide precision is exhausted. */
        while (count < limit) {
            size_t n = mbrtowc(NULL, bytes + count, 1, &state);
            if (n == 0)
                break;
            if (n != 1)
                return fail(EILSEQ);
            ++count;
        }
    }
    return text(sink, wide, bytes, count, field);
}

static int integer(struct wide_sink *sink, uintmax_t value, unsigned base,
                   wchar_t sign, int uppercase, int pointer,
                   const struct field *field)
{
    const wchar_t *digits = uppercase ? L"0123456789ABCDEF" : L"0123456789abcdef";
    wchar_t storage[sizeof(value) * CHAR_BIT];
    wchar_t *end = storage + sizeof(storage) / sizeof(*storage);
    wchar_t *start = end;
    int prefix = pointer || ((field->flags & ALT) && base == 16 && value != 0);
    if (value != 0 || field->precision != 0 || pointer) {
        do {
            *--start = digits[value % base];
            value /= base;
        } while (value != 0);
    }
    size_t count = (size_t)(end - start);
    size_t zeros = field->precision > 0 && (size_t)field->precision > count
                   ? (size_t)field->precision - count : 0;
    if ((field->flags & ALT) && base == 8 && zeros == 0 &&
        (count == 0 || *start != L'0'))
        zeros = 1;
    size_t total = count + zeros + (sign != 0) + (prefix ? 2 : 0);
    size_t spaces = padding(total, field);
    if (reserve(sink, total + spaces) < 0)
        return -1;
    if ((field->flags & (ZERO | LEFT)) == ZERO && field->precision < 0) {
        zeros += spaces;
        spaces = 0;
    }
    if ((!(field->flags & LEFT) && repeat(sink, L' ', spaces) < 0) ||
        (sign != 0 && put(sink, sign) < 0) ||
        (prefix && (put(sink, L'0') < 0 || put(sink, uppercase ? L'X' : L'x') < 0)) ||
        repeat(sink, L'0', zeros) < 0)
        return -1;
    while (start != end) {
        if (put(sink, *start++) < 0)
            return -1;
    }
    return field->flags & LEFT ? repeat(sink, L' ', spaces) : 0;
}

/* In this runtime's LP64 targets, size_t and the unsigned counterpart of
   ptrdiff_t are unsigned long, and the signed counterpart of size_t is long. */
static intmax_t signed_arg(va_list *args, enum length length)
{
    switch (length) {
    case HH: return (signed char)va_arg(*args, int);
    case H: return (short)va_arg(*args, int);
    case L: return va_arg(*args, long);
    case LL: return va_arg(*args, long long);
    case J: return va_arg(*args, intmax_t);
    case Z: return va_arg(*args, long);
    case T: return va_arg(*args, ptrdiff_t);
    default: return va_arg(*args, int);
    }
}

static uintmax_t unsigned_arg(va_list *args, enum length length)
{
    switch (length) {
    case HH: return (unsigned char)va_arg(*args, int);
    case H: return (unsigned short)va_arg(*args, int);
    case L: case T: return va_arg(*args, unsigned long);
    case LL: return va_arg(*args, unsigned long long);
    case J: return va_arg(*args, uintmax_t);
    case Z: return va_arg(*args, size_t);
    default: return va_arg(*args, unsigned int);
    }
}

static void store_count(va_list *args, enum length length, int count)
{
    switch (length) {
    case HH: *va_arg(*args, signed char *) = (signed char)count; break;
    case H: *va_arg(*args, short *) = (short)count; break;
    case L: case Z: *va_arg(*args, long *) = count; break;
    case LL: *va_arg(*args, long long *) = count; break;
    case J: *va_arg(*args, intmax_t *) = count; break;
    case T: *va_arg(*args, ptrdiff_t *) = count; break;
    default: *va_arg(*args, int *) = count; break;
    }
}

static int decimal(const wchar_t **format)
{
    int value = 0;
    while (**format >= L'0' && **format <= L'9') {
        int digit = (int)(**format - L'0');
        if (value > (INT_MAX - digit) / 10)
            return fail(EOVERFLOW);
        value = 10 * value + digit;
        ++*format;
    }
    return value;
}

static int conversion(struct wide_sink *sink, const wchar_t **cursor, va_list *args)
{
    const wchar_t *format = *cursor;
    struct field field = {0, 0, -1, DEFAULT};
    if (*format == L'%') {
        *cursor = format + 1;
        return reserve(sink, 1) < 0 ? -1 : put(sink, L'%');
    }
    for (;;) {
        switch (*format) {
        case L'-': field.flags |= LEFT; break;
        case L'+': field.flags |= PLUS; break;
        case L' ': field.flags |= SPACE; break;
        case L'#': field.flags |= ALT; break;
        case L'0': field.flags |= ZERO; break;
        default: goto width;
        }
        ++format;
    }
width:;
    int has_width = *format == L'*' || (*format >= L'1' && *format <= L'9');
    if (*format == L'*') {
        ++format;
        field.width = va_arg(*args, int);
        if (field.width < 0) {
            if (field.width == INT_MIN)
                return fail(EOVERFLOW);
            field.flags |= LEFT;
            field.width = -field.width;
        }
    } else if ((field.width = decimal(&format)) < 0) {
        return -1;
    }
    int has_precision = *format == L'.';
    if (has_precision) {
        ++format;
        if (*format == L'*') {
            ++format;
            field.precision = va_arg(*args, int);
        } else if ((field.precision = decimal(&format)) < 0) {
            return -1;
        }
    }
    switch (*format) {
    case L'h': ++format; field.length = *format == L'h' ? (++format, HH) : H; break;
    case L'l': ++format; field.length = *format == L'l' ? (++format, LL) : L; break;
    case L'j': ++format; field.length = J; break;
    case L'z': ++format; field.length = Z; break;
    case L't': ++format; field.length = T; break;
    case L'L': ++format; field.length = BIG_L; break;
    }
    if (*format == L'\0')
        return fail(EINVAL);
    *cursor = format + 1;
    enum length length = field.length;
    switch (*format) {
    case L'd': case L'i': {
        if (length == BIG_L || (field.flags & ALT))
            break;
        intmax_t value = signed_arg(args, length);
        uintmax_t mag = value < 0 ? (uintmax_t)0 - (uintmax_t)value : (uintmax_t)value;
        wchar_t sign = value < 0 ? L'-' : (field.flags & PLUS) ? L'+' :
                       (field.flags & SPACE) ? L' ' : 0;
        return integer(sink, mag, 10, sign, 0, 0, &field);
    }
    case L'u': case L'o': case L'x': case L'X': {
        if (length == BIG_L || (field.flags & (PLUS | SPACE)) ||
            (*format == L'u' && (field.flags & ALT)))
            break;
        unsigned base = *format == L'u' ? 10 : *format == L'o' ? 8 : 16;
        return integer(sink, unsigned_arg(args, length), base, 0, *format == L'X', 0, &field);
    }
    case L'p':
        if (length == DEFAULT && !has_precision && !(field.flags & ~LEFT))
            return integer(sink, (uintptr_t)va_arg(*args, void *), 16, 0, 0, 1, &field);
        break;
    case L'n':
        if (length == BIG_L || field.flags || has_width || has_precision)
            break;
        store_count(args, length, (int)sink->count);
        return 0;
    case L'c': {
        if ((length != DEFAULT && length != L) || has_precision || (field.flags & ~LEFT))
            break;
        wchar_t wc;
        if (length == L) {
            wc = (wchar_t)va_arg(*args, wint_t);
        } else {
            wint_t converted = btowc(va_arg(*args, int));
            if (converted == WEOF)
                return fail(EILSEQ);
            wc = (wchar_t)converted;
        }
        return text(sink, &wc, NULL, 1, &field);
    }
    case L's':
        if (field.flags & ~LEFT)
            break;
        if (length == L)
            return string(sink, va_arg(*args, wchar_t *), NULL, &field);
        if (length == DEFAULT)
            return string(sink, NULL, va_arg(*args, char *), &field);
        break;
    case L'a': case L'A': case L'e': case L'E':
    case L'f': case L'F': case L'g': case L'G':
        if (length == BIG_L)
            return fail(ENOSYS);
        if (length == DEFAULT || length == L) {
            struct fp_sink fp = {sink, fp_wide_reserve, fp_wide_write, fp_wide_repeat};
            return __cephyr_printf_float(&fp, va_arg(*args, double), (char)*format,
                                         field.flags & ALT, field.flags & PLUS,
                                         field.flags & SPACE, field.flags & LEFT,
                                         field.flags & ZERO, field.width, field.precision);
        }
        break;
    }
    return fail(EINVAL);
}

static int format_into(struct wide_sink *sink, const wchar_t *format, va_list incoming)
{
    va_list args;
    va_copy(args, incoming);
    int status = 0;
    while (*format != L'\0' && status == 0) {
        if (*format == L'%') {
            ++format;
            status = conversion(sink, &format, &args);
        } else {
            status = reserve(sink, 1) < 0 ? -1 : put(sink, *format++);
        }
    }
    va_end(args);
    return status < 0 ? -1 : (int)sink->count;
}

int vfwprintf(FILE *restrict stream, const wchar_t *restrict format, va_list args)
{
    if (__cephyr_stdio_orient(stream, CEPHYR_STREAM_WIDE) == EOF)
        return -1;
    struct wide_sink sink = {.stream = stream};
    return format_into(&sink, format, args);
}

int vswprintf(wchar_t *restrict buffer, size_t size,
              const wchar_t *restrict format, va_list args)
{
    if (size == 0)
        return fail(EOVERFLOW);
    struct wide_sink sink = {.buffer = buffer, .capacity = size};
    int result = format_into(&sink, format, args);
    buffer[sink.count] = L'\0';
    return result;
}

int vwprintf(const wchar_t *restrict format, va_list args)
{
    return vfwprintf(stdout, format, args);
}

int fwprintf(FILE *restrict stream, const wchar_t *restrict format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfwprintf(stream, format, args);
    va_end(args);
    return result;
}

int swprintf(wchar_t *restrict buffer, size_t size, const wchar_t *restrict format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vswprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

int wprintf(const wchar_t *restrict format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vwprintf(format, args);
    va_end(args);
    return result;
}
