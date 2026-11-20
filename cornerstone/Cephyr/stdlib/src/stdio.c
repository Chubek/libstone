#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define STREAM_EOF 1U
#define STREAM_ERROR 2U

struct __cephyr_FILE {
    int descriptor;
    unsigned state;
};

static struct __cephyr_FILE stdin_object = {0, 0};
static struct __cephyr_FILE stdout_object = {1, 0};
static struct __cephyr_FILE stderr_object = {2, 0};

FILE *stdin = &stdin_object;
FILE *stdout = &stdout_object;
FILE *stderr = &stderr_object;

int fflush(FILE *stream)
{
    (void)stream;
    return 0;
}

void __cephyr_stdio_flush_all(void)
{
    (void)fflush(NULL);
}

void clearerr(FILE *stream)
{
    if (stream != NULL)
        stream->state = 0;
}

int feof(FILE *stream)
{
    return stream != NULL && (stream->state & STREAM_EOF) != 0;
}

int ferror(FILE *stream)
{
    return stream != NULL && (stream->state & STREAM_ERROR) != 0;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream)
{
    const unsigned char *data = (const unsigned char *)ptr;
    size_t total;
    size_t written = 0;

    if (size == 0 || count == 0)
        return 0;
    if (stream == NULL || count > SIZE_MAX / size) {
        errno = EINVAL;
        if (stream != NULL)
            stream->state |= STREAM_ERROR;
        return 0;
    }
    total = size * count;
    while (written < total) {
        ssize_t result =
            write(stream->descriptor, data + written, total - written);
        if (result > 0) {
            written += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        stream->state |= STREAM_ERROR;
        break;
    }
    return written / size;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *stream)
{
    unsigned char *data = (unsigned char *)ptr;
    size_t total;
    size_t received = 0;

    if (size == 0 || count == 0)
        return 0;
    if (stream == NULL || count > SIZE_MAX / size) {
        errno = EINVAL;
        if (stream != NULL)
            stream->state |= STREAM_ERROR;
        return 0;
    }
    if (feof(stream))
        return 0;
    total = size * count;
    while (received < total) {
        ssize_t result =
            read(stream->descriptor, data + received, total - received);
        if (result > 0) {
            received += (size_t)result;
            continue;
        }
        if (result == 0) {
            stream->state |= STREAM_EOF;
            break;
        }
        if (errno == EINTR)
            continue;
        stream->state |= STREAM_ERROR;
        break;
    }
    return received / size;
}

int fputc(int ch, FILE *stream)
{
    unsigned char byte = (unsigned char)ch;
    return fwrite(&byte, 1, 1, stream) == 1 ? (int)byte : EOF;
}

int putc(int ch, FILE *stream)
{
    return fputc(ch, stream);
}

int putchar(int ch)
{
    return fputc(ch, stdout);
}

int fgetc(FILE *stream)
{
    unsigned char byte;
    return fread(&byte, 1, 1, stream) == 1 ? (int)byte : EOF;
}

int getc(FILE *stream)
{
    return fgetc(stream);
}

int getchar(void)
{
    return fgetc(stdin);
}

char *fgets(char *str, int count, FILE *stream)
{
    int used = 0;

    if (str == NULL || count <= 0)
        return NULL;
    if (count == 1) {
        str[0] = '\0';
        return str;
    }
    while (used + 1 < count) {
        int ch = fgetc(stream);
        if (ch == EOF) {
            if (!feof(stream))
                return NULL;
            break;
        }
        str[used++] = (char)ch;
        if (ch == '\n')
            break;
    }
    if (used == 0)
        return NULL;
    str[used] = '\0';
    return str;
}

int fputs(const char *str, FILE *stream)
{
    size_t length = strlen(str);
    return fwrite(str, 1, length, stream) == length ? 0 : EOF;
}

int puts(const char *str)
{
    if (fputs(str, stdout) == EOF || fputc('\n', stdout) == EOF)
        return EOF;
    return 0;
}

void perror(const char *prefix)
{
    int saved_errno = errno;
    if (prefix != NULL && *prefix != '\0') {
        (void)fputs(prefix, stderr);
        (void)fputs(": ", stderr);
    }
    (void)fputs(strerror(saved_errno), stderr);
    (void)fputc('\n', stderr);
}

struct output_sink {
    char *buffer;
    size_t capacity;
    size_t count;
    FILE *stream;
    int failed;
};

static int sink_accepts(struct output_sink *sink, size_t length)
{
    if (sink->failed)
        return 0;
    if (length > (size_t)INT_MAX - sink->count) {
        sink->failed = 1;
        errno = EOVERFLOW;
        return 0;
    }
    return 1;
}

static void sink_write(struct output_sink *sink, const char *data,
                       size_t length)
{
    size_t index;

    if (!sink_accepts(sink, length))
        return;
    if (sink->stream != NULL) {
        if (fwrite(data, 1, length, sink->stream) != length) {
            sink->failed = 1;
            return;
        }
    } else if (sink->capacity != 0 && sink->buffer != NULL) {
        size_t available =
            sink->count < sink->capacity - 1
                ? (sink->capacity - 1) - sink->count
                : 0;
        size_t copy = length < available ? length : available;
        for (index = 0; index < copy; ++index)
            sink->buffer[sink->count + index] = data[index];
    }
    sink->count += length;
}

static void sink_repeat(struct output_sink *sink, char ch, size_t count)
{
    if (count == 0 || !sink_accepts(sink, count))
        return;

    if (sink->stream != NULL) {
        char block[64];
        memset(block, ch, sizeof(block));
        while (count != 0 && !sink->failed) {
            size_t amount = count < sizeof(block) ? count : sizeof(block);
            sink_write(sink, block, amount);
            count -= amount;
        }
    } else {
        if (sink->capacity != 0 && sink->count < sink->capacity - 1) {
            size_t available = (sink->capacity - 1) - sink->count;
            size_t fill = count < available ? count : available;
            memset(sink->buffer + sink->count, ch, fill);
        }
        sink->count += count;
    }
}

static unsigned long long signed_magnitude(long long value)
{
    unsigned long long converted = (unsigned long long)value;
    return value < 0 ? 0ULL - converted : converted;
}

static void emit_integer(struct output_sink *sink, unsigned long long value,
                         int negative, unsigned base, int uppercase,
                         int alternate, int plus, int blank, int left,
                         int zero, int width, int precision)
{
    char buffer[64];
    char *end = buffer + sizeof(buffer);
    char *start = end;
    const char *digits =
        uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    const char *prefix = "";
    size_t prefix_length = 0;
    size_t digit_count;
    size_t precision_zeroes;
    size_t total;
    size_t padding;
    char sign = '\0';
    int was_nonzero = value != 0;

    if (value != 0 || precision != 0) {
        do {
            *--start = digits[value % base];
            value /= base;
        } while (value != 0);
    }
    digit_count = (size_t)(end - start);
    if (negative)
        sign = '-';
    else if (plus)
        sign = '+';
    else if (blank)
        sign = ' ';

    if (alternate && base == 16 && (was_nonzero || alternate > 1)) {
        prefix = uppercase ? "0X" : "0x";
        prefix_length = 2;
    }

    precision_zeroes =
        precision > 0 && (size_t)precision > digit_count
            ? (size_t)precision - digit_count
            : 0;
    if (alternate && base == 8 && (digit_count == 0 || was_nonzero) &&
        precision_zeroes == 0)
        precision_zeroes = 1;
    total = digit_count + precision_zeroes + prefix_length +
            (sign != '\0' ? 1U : 0U);
    padding = width > 0 && (size_t)width > total ? (size_t)width - total : 0;
    if (!sink_accepts(sink, total + padding))
        return;

    if (!left && (!zero || precision >= 0))
        sink_repeat(sink, ' ', padding);
    if (sign != '\0')
        sink_write(sink, &sign, 1);
    if (prefix_length != 0)
        sink_write(sink, prefix, prefix_length);
    if (!left && zero && precision < 0)
        sink_repeat(sink, '0', padding);
    sink_repeat(sink, '0', precision_zeroes);
    sink_write(sink, start, digit_count);
    if (left)
        sink_repeat(sink, ' ', padding);
}

enum length_modifier {
    LENGTH_DEFAULT,
    LENGTH_HH,
    LENGTH_H,
    LENGTH_L,
    LENGTH_LL,
    LENGTH_J,
    LENGTH_Z,
    LENGTH_T
};

static unsigned long long read_unsigned(va_list *args,
                                        enum length_modifier length)
{
    switch (length) {
    case LENGTH_HH:
        return (unsigned char)va_arg(*args, int);
    case LENGTH_H:
        return (unsigned short)va_arg(*args, int);
    case LENGTH_L:
        return va_arg(*args, unsigned long);
    case LENGTH_LL:
        return va_arg(*args, unsigned long long);
    case LENGTH_J:
        return va_arg(*args, uintmax_t);
    case LENGTH_Z:
        return va_arg(*args, size_t);
    case LENGTH_T:
        return va_arg(*args, uintptr_t);
    default:
        return va_arg(*args, unsigned);
    }
}

static long long read_signed(va_list *args, enum length_modifier length)
{
    switch (length) {
    case LENGTH_HH:
        return (signed char)va_arg(*args, int);
    case LENGTH_H:
        return (short)va_arg(*args, int);
    case LENGTH_L:
        return va_arg(*args, long);
    case LENGTH_LL:
        return va_arg(*args, long long);
    case LENGTH_J:
        return va_arg(*args, intmax_t);
    case LENGTH_Z:
        return (long long)va_arg(*args, ptrdiff_t);
    case LENGTH_T:
        return va_arg(*args, ptrdiff_t);
    default:
        return va_arg(*args, int);
    }
}

static int parse_decimal(const char **cursor)
{
    int value = 0;
    while (**cursor >= '0' && **cursor <= '9') {
        int digit = **cursor - '0';
        if (value > (INT_MAX - digit) / 10)
            return -1;
        value = value * 10 + digit;
        ++*cursor;
    }
    return value;
}

static int format_to_sink(struct output_sink *sink, const char *format,
                          va_list incoming)
{
    va_list args;
    va_copy(args, incoming);

    while (*format != '\0' && !sink->failed) {
        int left = 0;
        int plus = 0;
        int blank = 0;
        int alternate = 0;
        int zero = 0;
        int width = 0;
        int precision = -1;
        enum length_modifier length = LENGTH_DEFAULT;
        char conversion;

        if (*format != '%') {
            const char *start = format;
            while (*format != '\0' && *format != '%')
                ++format;
            sink_write(sink, start, (size_t)(format - start));
            continue;
        }
        ++format;
        for (;;) {
            if (*format == '-')
                left = 1;
            else if (*format == '+')
                plus = 1;
            else if (*format == ' ')
                blank = 1;
            else if (*format == '#')
                alternate = 1;
            else if (*format == '0')
                zero = 1;
            else
                break;
            ++format;
        }
        if (*format == '*') {
            width = va_arg(args, int);
            ++format;
            if (width < 0) {
                if (width == INT_MIN) {
                    sink->failed = 1;
                    errno = EOVERFLOW;
                    break;
                }
                left = 1;
                width = -width;
            }
        } else {
            width = parse_decimal(&format);
            if (width < 0) {
                sink->failed = 1;
                errno = EOVERFLOW;
                break;
            }
        }
        if (*format == '.') {
            ++format;
            if (*format == '*') {
                precision = va_arg(args, int);
                ++format;
                if (precision < 0)
                    precision = -1;
            } else {
                precision = parse_decimal(&format);
                if (precision < 0) {
                    sink->failed = 1;
                    errno = EOVERFLOW;
                    break;
                }
            }
        }
        if (*format == 'h') {
            ++format;
            length = LENGTH_H;
            if (*format == 'h') {
                ++format;
                length = LENGTH_HH;
            }
        } else if (*format == 'l') {
            ++format;
            length = LENGTH_L;
            if (*format == 'l') {
                ++format;
                length = LENGTH_LL;
            }
        } else if (*format == 'j') {
            ++format;
            length = LENGTH_J;
        } else if (*format == 'z') {
            ++format;
            length = LENGTH_Z;
        } else if (*format == 't') {
            ++format;
            length = LENGTH_T;
        }

        conversion = *format;
        if (conversion != '\0')
            ++format;
        if (conversion == 'd' || conversion == 'i') {
            long long value = read_signed(&args, length);
            emit_integer(sink, signed_magnitude(value), value < 0, 10, 0, 0,
                         plus, blank, left, zero, width, precision);
        } else if (conversion == 'u' || conversion == 'o' ||
                   conversion == 'x' || conversion == 'X') {
            unsigned base =
                conversion == 'o' ? 8U
                                  : (conversion == 'u' ? 10U : 16U);
            emit_integer(sink, read_unsigned(&args, length), 0, base,
                         conversion == 'X', alternate, 0, 0, left, zero,
                         width, precision);
        } else if (conversion == 'p' && length == LENGTH_DEFAULT) {
            uintptr_t pointer = (uintptr_t)va_arg(args, void *);
            emit_integer(sink, pointer, 0, 16, 0, 2, 0, 0, left, zero, width,
                         precision < 0 ? 1 : precision);
        } else if (conversion == 'c' && length == LENGTH_DEFAULT) {
            char ch = (char)va_arg(args, int);
            size_t padding = width > 1 ? (size_t)width - 1 : 0;
            if (!sink_accepts(sink, 1 + padding))
                break;
            if (!left)
                sink_repeat(sink, ' ', padding);
            sink_write(sink, &ch, 1);
            if (left)
                sink_repeat(sink, ' ', padding);
        } else if (conversion == 's' && length == LENGTH_DEFAULT) {
            const char *str = va_arg(args, void *);
            size_t length_value;
            size_t padding;
            if (str == NULL)
                str = "(null)";
            length_value =
                precision >= 0 ? strnlen(str, (size_t)precision) : strlen(str);
            padding = width > 0 && (size_t)width > length_value
                          ? (size_t)width - length_value
                          : 0;
            if (!sink_accepts(sink, length_value + padding))
                break;
            if (!left)
                sink_repeat(sink, ' ', padding);
            sink_write(sink, str, length_value);
            if (left)
                sink_repeat(sink, ' ', padding);
        } else if (conversion == '%' && length == LENGTH_DEFAULT) {
            sink_write(sink, "%", 1);
        } else if (conversion == 'n') {
            if (length == LENGTH_HH)
                *va_arg(args, signed char *) = (signed char)sink->count;
            else if (length == LENGTH_H)
                *va_arg(args, short *) = (short)sink->count;
            else if (length == LENGTH_L)
                *va_arg(args, long *) = (long)sink->count;
            else if (length == LENGTH_LL)
                *va_arg(args, long long *) = (long long)sink->count;
            else if (length == LENGTH_J)
                *va_arg(args, intmax_t *) = (intmax_t)sink->count;
            else if (length == LENGTH_Z)
                *va_arg(args, ptrdiff_t *) = (ptrdiff_t)sink->count;
            else if (length == LENGTH_T)
                *va_arg(args, ptrdiff_t *) = (ptrdiff_t)sink->count;
            else
                *va_arg(args, int *) = (int)sink->count;
        } else {
            sink->failed = 1;
            errno = EINVAL;
        }
    }
    va_end(args);

    if (sink->stream == NULL && sink->capacity != 0 &&
        sink->buffer != NULL) {
        size_t terminator =
            sink->count < sink->capacity ? sink->count : sink->capacity - 1;
        sink->buffer[terminator] = '\0';
    }
    if (sink->failed)
        return -1;
    return (int)sink->count;
}

int vsnprintf(char *buffer, size_t size, const char *format, va_list args)
{
    struct output_sink sink;
    sink.buffer = buffer;
    sink.capacity = size;
    sink.count = 0;
    sink.stream = NULL;
    sink.failed = size != 0 && buffer == NULL;
    if (sink.failed)
        errno = EINVAL;
    return format_to_sink(&sink, format, args);
}

int snprintf(char *buffer, size_t size, const char *format, ...)
{
    int result;
    va_list args;
    va_start(args, format);
    result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

int vsprintf(char *buffer, const char *format, va_list args)
{
    return vsnprintf(buffer, (size_t)INT_MAX + 1, format, args);
}

int sprintf(char *buffer, const char *format, ...)
{
    int result;
    va_list args;
    va_start(args, format);
    result = vsprintf(buffer, format, args);
    va_end(args);
    return result;
}

int vfprintf(FILE *stream, const char *format, va_list args)
{
    struct output_sink sink;
    sink.buffer = NULL;
    sink.capacity = 0;
    sink.count = 0;
    sink.stream = stream;
    sink.failed = stream == NULL;
    if (stream == NULL)
        errno = EINVAL;
    return format_to_sink(&sink, format, args);
}

int fprintf(FILE *stream, const char *format, ...)
{
    int result;
    va_list args;
    va_start(args, format);
    result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int vprintf(const char *format, va_list args)
{
    return vfprintf(stdout, format, args);
}

int printf(const char *format, ...)
{
    int result;
    va_list args;
    va_start(args, format);
    result = vprintf(format, args);
    va_end(args);
    return result;
}
