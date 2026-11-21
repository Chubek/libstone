#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { NO_LOOKAHEAD = EOF - 1 };

struct scan_input {
    FILE *stream;
    const unsigned char *text;
    size_t consumed;
    int next;
};

static int scan_peek(struct scan_input *input)
{
    if (input->next == NO_LOOKAHEAD) {
        if (input->stream != NULL)
            input->next = fgetc(input->stream);
        else
            input->next = *input->text != 0 ? *input->text : EOF;
    }
    return input->next;
}

/* Called only to consume a known non-EOF byte from scan_peek. */
static void scan_consume(struct scan_input *input)
{
    if (input->stream == NULL)
        ++input->text;
    if (input->consumed < SIZE_MAX)
        ++input->consumed;
    input->next = NO_LOOKAHEAD;
}

static void scan_space(struct scan_input *input)
{
    while (isspace(scan_peek(input)))
        scan_consume(input);
}

enum scan_result {
    SCAN_OK,
    SCAN_MATCH_FAILURE,
    SCAN_INPUT_FAILURE,
    SCAN_DIAGNOSTIC
};

static enum scan_result scan_error(int error)
{
    errno = error;
    return SCAN_DIAGNOSTIC;
}

static enum scan_result scan_literal(struct scan_input *input, int expected)
{
    int ch = scan_peek(input);
    if (ch != expected)
        return ch == EOF ? SCAN_INPUT_FAILURE : SCAN_MATCH_FAILURE;
    scan_consume(input);
    return SCAN_OK;
}

enum scan_length {
    SCAN_INT, SCAN_HH, SCAN_H, SCAN_L, SCAN_LL, SCAN_J, SCAN_Z, SCAN_T, SCAN_LD
};

struct scan_conversion {
    size_t width;
    int suppress;
    enum scan_length length;
    char kind;
    int invert;
    unsigned char set[UCHAR_MAX + 1];
};

static enum scan_length scan_length_modifier(const char **format)
{
    switch (**format) {
    case 'h':
        ++*format;
        if (**format != 'h')
            return SCAN_H;
        ++*format;
        return SCAN_HH;
    case 'l':
        ++*format;
        if (**format != 'l')
            return SCAN_L;
        ++*format;
        return SCAN_LL;
    case 'j': ++*format; return SCAN_J;
    case 'z': ++*format; return SCAN_Z;
    case 't': ++*format; return SCAN_T;
    case 'L': ++*format; return SCAN_LD;
    default: return SCAN_INT;
    }
}

static enum scan_result scan_set(const char **format,
                                 struct scan_conversion *conversion)
{
    const unsigned char *start = (const unsigned char *)*format;
    conversion->invert = *start == '^';
    start += conversion->invert;
    const unsigned char *end = start + (*start == ']');
    while (*end != 0 && *end != ']')
        ++end;
    if (*end == 0)
        return scan_error(EINVAL);
    *format = (const char *)end + 1;

    memset(conversion->set, 0, sizeof(conversion->set));
    for (const unsigned char *p = start; p != end; ++p) {
        /* Cephyr defines ascending byte ranges; a hyphen without two such
           endpoints is literal. Consume each range from left to right. */
        if (end - p >= 3 && p[0] != '-' && p[1] == '-' &&
            p[2] != '-' && p[0] <= p[2]) {
            for (int ch = p[0]; ch <= p[2]; ++ch)
                conversion->set[ch] = 1;
            p += 2;
        } else {
            conversion->set[*p] = 1;
        }
    }
    return SCAN_OK;
}

static enum scan_result scan_spec(const char **format,
                                 struct scan_conversion *conversion)
{
    const char *p = *format;
    conversion->suppress = *p == '*';
    p += conversion->suppress;
    conversion->width = 0;
    if (isdigit((unsigned char)*p)) {
        do {
            unsigned digit = *p++ - '0';
            if (conversion->width > (SIZE_MAX - digit) / 10)
                return scan_error(EOVERFLOW);
            conversion->width = conversion->width * 10 + digit;
        } while (isdigit((unsigned char)*p));
        if (conversion->width == 0)
            return scan_error(EINVAL);
    }
    conversion->length = scan_length_modifier(&p);
    conversion->kind = *p;
    if (*p != '\0')
        ++p;
    *format = p;

    switch (conversion->kind) {
    case 'n':
        if (conversion->width != 0 || conversion->suppress)
            break;
        /* fall through */
    case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
        if (conversion->length != SCAN_LD)
            return SCAN_OK;
        break;
    case 'c': case 's': case '[':
        if (conversion->length != SCAN_INT && conversion->length != SCAN_L)
            break;
        return conversion->kind == '[' ? scan_set(format, conversion) : SCAN_OK;
    case '%':
        if (conversion->width != 0 || conversion->suppress)
            break;
        /* fall through */
    case 'p':
        if (conversion->length == SCAN_INT)
            return SCAN_OK;
        break;
    case 'a': case 'A': case 'e': case 'E':
    case 'f': case 'F': case 'g': case 'G':
        if (conversion->length == SCAN_INT || conversion->length == SCAN_L ||
            conversion->length == SCAN_LD)
            return scan_error(ENOSYS);
        break;
    }
    return scan_error(EINVAL);
}

static const struct {
    unsigned long long signed_max;
    unsigned long long unsigned_max;
} integer_limits[] = {
    [SCAN_INT] = { INT_MAX, UINT_MAX },
    [SCAN_HH] = { SCHAR_MAX, UCHAR_MAX },
    [SCAN_H] = { SHRT_MAX, USHRT_MAX },
    [SCAN_L] = { LONG_MAX, ULONG_MAX },
    [SCAN_LL] = { LLONG_MAX, ULLONG_MAX },
    [SCAN_J] = { INTMAX_MAX, UINTMAX_MAX },
    /* Bundled LP64 types: size_t is unsigned long, ptrdiff_t is long. */
    [SCAN_Z] = { LONG_MAX, SIZE_MAX },
    [SCAN_T] = { PTRDIFF_MAX, ULONG_MAX }
};

static enum scan_result store_signed(va_list *args, enum scan_length length,
                                     unsigned long long magnitude, int negative)
{
    if (magnitude > integer_limits[length].signed_max + (unsigned)negative)
        return scan_error(ERANGE);
    long long value = negative && magnitude != 0
                          ? -(long long)(magnitude - 1) - 1
                          : (long long)magnitude;
    switch (length) {
    case SCAN_HH: *va_arg(*args, signed char *) = (signed char)value; break;
    case SCAN_H: *va_arg(*args, short *) = (short)value; break;
    case SCAN_L: case SCAN_Z: *va_arg(*args, long *) = (long)value; break;
    case SCAN_LL: *va_arg(*args, long long *) = value; break;
    case SCAN_J: *va_arg(*args, intmax_t *) = (intmax_t)value; break;
    case SCAN_T: *va_arg(*args, ptrdiff_t *) = (ptrdiff_t)value; break;
    default: *va_arg(*args, int *) = (int)value; break;
    }
    return SCAN_OK;
}

static enum scan_result store_unsigned(va_list *args, enum scan_length length,
                                       unsigned long long magnitude, int negative)
{
    if (magnitude > integer_limits[length].unsigned_max)
        return scan_error(ERANGE);
    unsigned long long value = negative ? 0ULL - magnitude : magnitude;
    switch (length) {
    case SCAN_HH: *va_arg(*args, unsigned char *) = (unsigned char)value; break;
    case SCAN_H: *va_arg(*args, unsigned short *) = (unsigned short)value; break;
    case SCAN_L: case SCAN_T:
        *va_arg(*args, unsigned long *) = (unsigned long)value;
        break;
    case SCAN_LL: *va_arg(*args, unsigned long long *) = value; break;
    case SCAN_J: *va_arg(*args, uintmax_t *) = (uintmax_t)value; break;
    case SCAN_Z: *va_arg(*args, size_t *) = (size_t)value; break;
    default: *va_arg(*args, unsigned *) = (unsigned)value; break;
    }
    return SCAN_OK;
}

static int scan_digit(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

static enum scan_result scan_integer(struct scan_input *input,
                                     const struct scan_conversion *conversion,
                                     va_list *args)
{
    const char kind = conversion->kind;
    unsigned base = kind == 'i' ? 0 : kind == 'o' ? 8 :
                    kind == 'x' || kind == 'X' || kind == 'p' ? 16 : 10;
    size_t left = conversion->width != 0 ? conversion->width : SIZE_MAX;
    int ch = scan_peek(input);
    int negative = ch == '-';
    int any = 0;
    int overflow = 0;
    unsigned long long value = 0;

    if (ch == EOF)
        return SCAN_INPUT_FAILURE;
    if (kind != 'p' && (ch == '+' || negative)) {
        scan_consume(input);
        ch = --left != 0 ? scan_peek(input) : EOF;
    }
    if ((base == 0 || base == 16) && ch == '0') {
        scan_consume(input);
        ch = --left != 0 ? scan_peek(input) : EOF;
        any = 1;
        if (ch == 'x' || ch == 'X') {
            scan_consume(input);
            --left;
            base = 16;
            any = 0;
        } else if (kind == 'p') {
            return SCAN_MATCH_FAILURE;
        } else if (base == 0) {
            base = 8;
        }
    } else if (kind == 'p') {
        return SCAN_MATCH_FAILURE;
    }
    if (base == 0)
        base = 10;

    const unsigned long long cutoff = ULLONG_MAX / base;
    const unsigned cutlim = (unsigned)(ULLONG_MAX % base);
    while (left != 0) {
        int digit = scan_digit(scan_peek(input));
        if (digit < 0 || (unsigned)digit >= base)
            break;
        scan_consume(input);
        --left;
        any = 1;
        if (value > cutoff || (value == cutoff && (unsigned)digit > cutlim))
            overflow = 1;
        else if (!overflow)
            value = value * base + (unsigned)digit;
    }
    /* A lone sign or incomplete 0x is a consumed prefix, not a value. */
    if (!any)
        return SCAN_MATCH_FAILURE;
    if (conversion->suppress)
        return SCAN_OK;
    if (overflow)
        return scan_error(ERANGE);
    if (kind == 'p') {
        if (value > UINTPTR_MAX)
            return scan_error(ERANGE);
        *va_arg(*args, void **) = (void *)(uintptr_t)value;
        return SCAN_OK;
    }
    if (kind == 'd' || kind == 'i')
        return store_signed(args, conversion->length, value, negative);
    return store_unsigned(args, conversion->length, value, negative);
}

static enum scan_result scan_characters(struct scan_input *input,
                                        const struct scan_conversion *conversion,
                                        va_list *args)
{
    int fixed = conversion->kind == 'c';
    int wide = conversion->length == SCAN_L;
    int stage = fixed || wide;
    size_t width = conversion->width;
    size_t used = 0;
    int bad_encoding = 0;
    unsigned char local[64];
    unsigned char *heap = NULL;
    unsigned char *bytes = stage ? local : NULL;
    size_t capacity = sizeof(local);
    enum scan_result result = SCAN_OK;
    if (width == 0)
        width = fixed ? 1 : SIZE_MAX;

    /* Short %c fields and invalid wide fields must not commit an assignment.
       Stage these as bytes, growing only when actual input exceeds the
       local buffer. Successful narrow %s and %[ can write incrementally. */
    while (used < width) {
        int ch = scan_peek(input);
        if (ch == EOF || (conversion->kind == 's' && isspace(ch)) ||
            (conversion->kind == '[' &&
             conversion->set[ch] == conversion->invert))
            break;
        if (wide && btowc(ch) == WEOF)
            bad_encoding = 1;
        if (!conversion->suppress && !bad_encoding) {
            if (stage && used == capacity) {
                size_t more = capacity <= width / 2 ? capacity * 2 : width;
                int from_local = heap == NULL;
                unsigned char *grown = realloc(heap, more);
                if (grown == NULL) {
                    result = scan_error(ENOMEM);
                    goto done;
                }
                if (from_local)
                    memcpy(grown, local, used);
                bytes = heap = grown;
                capacity = more;
            } else if (!stage && used == 0) {
                /* void * covers all three character pointer types in va_arg. */
                bytes = va_arg(*args, void *);
            }
            bytes[used] = (unsigned char)ch;
        }
        scan_consume(input);
        ++used;
    }

    if (bad_encoding) {
        errno = EILSEQ;
        result = SCAN_INPUT_FAILURE;
    } else if (used == 0) {
        result = input->next == EOF ? SCAN_INPUT_FAILURE : SCAN_MATCH_FAILURE;
    } else if (fixed && used != width) {
        /* A nonempty short field is a prefix, not a matching input item. */
        result = SCAN_MATCH_FAILURE;
    } else if (!conversion->suppress) {
        if (wide) {
            wchar_t *out = va_arg(*args, wchar_t *);
            /* The bundled C encoding maps each valid ASCII byte to itself. */
            for (size_t i = 0; i < used; ++i)
                out[i] = (wchar_t)bytes[i];
            if (!fixed)
                out[used] = 0;
        } else if (fixed) {
            memcpy(va_arg(*args, void *), bytes, used);
        } else {
            bytes[used] = 0;
        }
    }
done:
    if (heap != NULL)
        free(heap);
    return result;
}

static int scan(struct scan_input *input, const char *format, va_list incoming)
{
    va_list args;
    int assignments = 0;
    int completed = 0;
    enum scan_result result = SCAN_OK;
    va_copy(args, incoming);

    while (*format != '\0') {
        unsigned char ch = (unsigned char)*format++;
        if (isspace(ch)) {
            while (isspace((unsigned char)*format))
                ++format;
            scan_space(input);
        } else if (ch != '%') {
            result = scan_literal(input, ch);
        } else {
            struct scan_conversion conversion;
            result = scan_spec(&format, &conversion);
            if (result != SCAN_OK)
                break;
            ch = (unsigned char)conversion.kind;
            if (ch != '[' && ch != 'c' && ch != 'n')
                scan_space(input);
            if (ch == '%') {
                result = scan_literal(input, '%');
            } else if (ch == 'n') {
                result = store_signed(&args, conversion.length,
                                      input->consumed, 0);
            } else if (ch == '[' || ch == 'c' || ch == 's') {
                result = scan_characters(input, &conversion, &args);
            } else {
                result = scan_integer(input, &conversion, &args);
            }
            if (result == SCAN_OK && ch != '%') {
                /* Suppression and %n complete a conversion without adding
                   to the return count; literal matches and %% do not. */
                completed = 1;
                if (!conversion.suppress && ch != 'n') {
                    if (assignments == INT_MAX)
                        result = scan_error(EOVERFLOW);
                    else
                        ++assignments;
                }
            }
        }
        if (result != SCAN_OK)
            break;
    }
    va_end(args);

    /* No seeker is needed, including on pipes. One pushback slot suffices
       and any pushback originally in the stream was read by scan_peek. */
    if (input->stream != NULL && input->next >= 0)
        (void)ungetc(input->next, input->stream);
    if ((result == SCAN_INPUT_FAILURE && !completed) ||
        (result == SCAN_DIAGNOSTIC && assignments == 0))
        return EOF;
    return assignments;
}

int vfscanf(FILE *stream, const char *format, va_list args)
{
    struct scan_input input = { stream, NULL, 0, NO_LOOKAHEAD };
    if (stream == NULL || format == NULL) {
        errno = EINVAL;
        return EOF;
    }
    return scan(&input, format, args);
}

int vscanf(const char *format, va_list args)
{
    return vfscanf(stdin, format, args);
}

int vsscanf(const char *text, const char *format, va_list args)
{
    struct scan_input input = { NULL, (const unsigned char *)text, 0, NO_LOOKAHEAD };
    if (text == NULL || format == NULL) {
        errno = EINVAL;
        return EOF;
    }
    return scan(&input, format, args);
}

int fscanf(FILE *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfscanf(stream, format, args);
    va_end(args);
    return result;
}

int scanf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vscanf(format, args);
    va_end(args);
    return result;
}

int sscanf(const char *text, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsscanf(text, format, args);
    va_end(args);
    return result;
}
