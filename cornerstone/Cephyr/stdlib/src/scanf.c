#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "stream.h"

struct scan_input {
    FILE *stream;
    const void *text;
    size_t consumed;
    wint_t next;
    int buffered;
    int wide;
};

static wint_t scan_peek(struct scan_input *input)
{
    if (!input->buffered) {
        if (input->stream != NULL) {
            if (input->wide) {
                input->next = fgetwc(input->stream);
            } else {
                int ch = fgetc(input->stream);
                input->next = ch == EOF ? WEOF : (wint_t)ch;
            }
        } else {
            wint_t ch = input->wide ? (wint_t)*(const wchar_t *)input->text
                                   : *(const unsigned char *)input->text;
            input->next = ch != 0 ? ch : WEOF;
        }
        input->buffered = 1;
    }
    return input->next;
}

/* Called only to consume a non-WEOF character returned by scan_peek. */
static void scan_consume(struct scan_input *input)
{
    if (input->stream == NULL)
        input->text = input->wide ? (const void *)((const wchar_t *)input->text + 1)
                                  : (const unsigned char *)input->text + 1;
    if (input->consumed < SIZE_MAX)
        ++input->consumed;
    input->buffered = 0;
}

static int scan_is_space(int wide, wint_t ch)
{
    return ch != WEOF && (wide ? iswspace(ch) : isspace((int)ch));
}

static void scan_space(struct scan_input *input)
{
    while (scan_is_space(input->wide, scan_peek(input)))
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

static enum scan_result scan_literal(struct scan_input *input, wint_t expected)
{
    wint_t ch = scan_peek(input);
    if (ch == WEOF)
        return SCAN_INPUT_FAILURE;
    if (ch != expected)
        return SCAN_MATCH_FAILURE;
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
    wint_t kind;
    int invert;
    unsigned char set[UCHAR_MAX + 1];
    const wchar_t *wide_set;
    size_t set_length;
};

struct scan_format {
    const void *text;
    size_t pos;
    int wide;
};

static wint_t scan_format_at(const struct scan_format *format, size_t offset)
{
    size_t index = format->pos + offset;
    return format->wide ? (wint_t)((const wchar_t *)format->text)[index]
                        : ((const unsigned char *)format->text)[index];
}

static enum scan_length scan_length_modifier(struct scan_format *format)
{
    switch (scan_format_at(format, 0)) {
    case 'h':
        ++format->pos;
        if (scan_format_at(format, 0) != 'h')
            return SCAN_H;
        ++format->pos;
        return SCAN_HH;
    case 'l':
        ++format->pos;
        if (scan_format_at(format, 0) != 'l')
            return SCAN_L;
        ++format->pos;
        return SCAN_LL;
    case 'j': ++format->pos; return SCAN_J;
    case 'z': ++format->pos; return SCAN_Z;
    case 't': ++format->pos; return SCAN_T;
    case 'L': ++format->pos; return SCAN_LD;
    default: return SCAN_INT;
    }
}

static enum scan_result scan_set(struct scan_format *format,
                                 struct scan_conversion *conversion)
{
    conversion->invert = scan_format_at(format, 0) == '^';
    format->pos += conversion->invert;
    size_t end = scan_format_at(format, 0) == ']';
    while (scan_format_at(format, end) != 0 && scan_format_at(format, end) != ']')
        ++end;
    if (scan_format_at(format, end) == 0)
        return scan_error(EINVAL);

    if (format->wide) {
        conversion->wide_set = (const wchar_t *)format->text + format->pos;
        conversion->set_length = end;
    } else {
        const unsigned char *start = (const unsigned char *)format->text + format->pos;
        memset(conversion->set, 0, sizeof(conversion->set));
        for (size_t i = 0; i < end; ++i) {
            /* Cephyr defines ascending ranges with non-hyphen endpoints.
               Consume each range from left to right. */
            if (end - i >= 3 && start[i] != '-' && start[i + 1] == '-' &&
                start[i + 2] != '-' && start[i] <= start[i + 2]) {
                for (int ch = start[i]; ch <= start[i + 2]; ++ch)
                    conversion->set[ch] = 1;
                i += 2;
            } else {
                conversion->set[start[i]] = 1;
            }
        }
    }
    format->pos += end + 1;
    return SCAN_OK;
}

static enum scan_result scan_spec(struct scan_format *format,
                                 struct scan_conversion *conversion)
{
    conversion->suppress = scan_format_at(format, 0) == '*';
    format->pos += conversion->suppress;
    conversion->width = 0;
    wint_t ch = scan_format_at(format, 0);
    if (ch >= '0' && ch <= '9') {
        do {
            unsigned digit = ch - '0';
            if (conversion->width > (SIZE_MAX - digit) / 10)
                return scan_error(EOVERFLOW);
            conversion->width = conversion->width * 10 + digit;
            ++format->pos;
            ch = scan_format_at(format, 0);
        } while (ch >= '0' && ch <= '9');
        if (conversion->width == 0)
            return scan_error(EINVAL);
    }
    conversion->length = scan_length_modifier(format);
    conversion->kind = scan_format_at(format, 0);
    if (conversion->kind != 0)
        ++format->pos;

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
        if (conversion->length == SCAN_LD)
            return scan_error(ENOSYS);
        if (conversion->length == SCAN_INT || conversion->length == SCAN_L)
            return format->wide ? scan_error(ENOSYS) : SCAN_OK;
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

static int scan_digit(wint_t ch)
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
    const wint_t kind = conversion->kind;
    unsigned base = kind == 'i' ? 0 : kind == 'o' ? 8 :
                    kind == 'x' || kind == 'X' || kind == 'p' ? 16 : 10;
    size_t left = conversion->width != 0 ? conversion->width : SIZE_MAX;
    wint_t ch = scan_peek(input);
    int negative = ch == '-';
    int any = 0;
    int overflow = 0;
    unsigned long long value = 0;

    if (ch == WEOF)
        return SCAN_INPUT_FAILURE;
    if (kind != 'p' && (ch == '+' || negative)) {
        scan_consume(input);
        ch = --left != 0 ? scan_peek(input) : WEOF;
    }
    if ((base == 0 || base == 16) && ch == '0') {
        scan_consume(input);
        ch = --left != 0 ? scan_peek(input) : WEOF;
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
        wint_t ch = scan_peek(input);
        if (ch == WEOF || (conversion->kind == 's' && isspace((int)ch)) ||
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
        result = input->next == WEOF ? SCAN_INPUT_FAILURE : SCAN_MATCH_FAILURE;
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

static int wide_set_contains(const struct scan_conversion *conversion, wint_t ch)
{
    const wchar_t *set = conversion->wide_set;
    for (size_t i = 0; i < conversion->set_length; ++i) {
        wint_t first = (wint_t)set[i];
        if (conversion->set_length - i >= 3 && first != '-' &&
            set[i + 1] == L'-' && set[i + 2] != L'-' &&
            first <= (wint_t)set[i + 2]) {
            if (first <= ch && ch <= (wint_t)set[i + 2])
                return 1;
            i += 2;
        } else if (ch == first) {
            return 1;
        }
    }
    return 0;
}

struct scan_field {
    unsigned char local[64];
    unsigned char *heap;
    size_t used;
    size_t capacity;
};

static enum scan_result scan_append(struct scan_field *field,
                                    const void *data, size_t size)
{
    if (size > SIZE_MAX - field->used)
        return scan_error(EOVERFLOW);
    size_t needed = field->used + size;
    if (needed > field->capacity) {
        size_t capacity = field->capacity <= SIZE_MAX / 2
                              ? field->capacity * 2 : SIZE_MAX;
        if (capacity < needed)
            capacity = needed;
        int from_local = field->heap == NULL;
        unsigned char *grown = realloc(field->heap, capacity);
        if (grown == NULL)
            return scan_error(ENOMEM);
        if (from_local)
            memcpy(grown, field->local, field->used);
        field->heap = grown;
        field->capacity = capacity;
    }
    unsigned char *bytes = field->heap != NULL ? field->heap : field->local;
    memcpy(bytes + field->used, data, size);
    field->used = needed;
    return SCAN_OK;
}

struct scan_fp_item {
    struct scan_input *input;
    struct scan_field field;
    size_t left;
    int suppress;
    enum scan_result result;
};

/* Fold ASCII for the C-locale grammar, but save the original byte for strtofp. */
static wint_t scan_fp_peek(struct scan_fp_item *item)
{
    if (item->left == 0 || item->result != SCAN_OK)
        return WEOF;
    wint_t ch = scan_peek(item->input);
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

static void scan_fp_consume(struct scan_fp_item *item)
{
    if (!item->suppress) {
        unsigned char byte = (unsigned char)item->input->next;
        item->result = scan_append(&item->field, &byte, 1);
        if (item->result != SCAN_OK)
            return;
    }
    scan_consume(item->input);
    --item->left;
}

static int scan_fp_accept(struct scan_fp_item *item, wint_t ch)
{
    if (scan_fp_peek(item) != ch)
        return 0;
    scan_fp_consume(item);
    return item->result == SCAN_OK;
}

static int scan_fp_word(struct scan_fp_item *item, const char *tail)
{
    for (; *tail; ++tail)
        if (!scan_fp_accept(item, (unsigned char)*tail))
            return 0;
    return 1;
}

/* §7.21.6.2: consume the longest prefix of a valid §7.22.1.3 subject
   sequence, not the longest complete sequence that strtof/strtod accept.
   Return whether the consumed prefix is itself complete. */
static int scan_fp_subject(struct scan_fp_item *item)
{
    wint_t ch = scan_fp_peek(item);
    if (ch == '+' || ch == '-')
        scan_fp_consume(item);
    if (scan_fp_accept(item, 'i')) {
        if (!scan_fp_word(item, "nf"))
            return 0;
        return !scan_fp_accept(item, 'i') || scan_fp_word(item, "nity");
    }
    if (scan_fp_accept(item, 'n')) {
        if (!scan_fp_word(item, "an"))
            return 0;
        if (!scan_fp_accept(item, '('))
            return 1;
        while ((ch = scan_fp_peek(item)) == '_' ||
               (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
            scan_fp_consume(item);
        return scan_fp_accept(item, ')');
    }

    int base = 10, point = 0;
    int any = scan_fp_accept(item, '0');
    if (any && scan_fp_accept(item, 'x')) {
        base = 16;
        any = 0;
    }
    for (;;) {
        ch = scan_fp_peek(item);
        if (ch == '.' && !point) {
            point = 1;
        } else {
            int digit = scan_digit(ch);
            if (digit < 0 || digit >= base)
                break;
            any = 1;
        }
        scan_fp_consume(item);
    }
    if (!any)
        return 0;
    if (!scan_fp_accept(item, base == 10 ? 'e' : 'p'))
        return 1;
    if (!scan_fp_accept(item, '+'))
        scan_fp_accept(item, '-');
    any = 0;
    while ((ch = scan_fp_peek(item)) >= '0' && ch <= '9') {
        scan_fp_consume(item);
        any = 1;
    }
    return any;
}

static enum scan_result scan_float(struct scan_input *input,
                                   const struct scan_conversion *conversion,
                                   va_list *args)
{
    if (scan_peek(input) == WEOF)
        return SCAN_INPUT_FAILURE;
    struct scan_fp_item item = {
        .input = input,
        .field = { .capacity = sizeof(item.field.local) },
        .left = conversion->width != 0 ? conversion->width : SIZE_MAX,
        .suppress = conversion->suppress,
        .result = SCAN_OK,
    };
    int complete = scan_fp_subject(&item);
    enum scan_result result = item.result;
    if (result == SCAN_OK && !complete)
        result = SCAN_MATCH_FAILURE;
    if (result == SCAN_OK && !item.suppress) {
        result = scan_append(&item.field, "", 1);
        if (result == SCAN_OK) {
            const unsigned char *bytes = item.field.heap != NULL ? item.field.heap : item.field.local;
            const char *text = (const char *)bytes;
            /* Convert at the destination precision to avoid double rounding. */
            if (conversion->length == SCAN_L)
                *va_arg(*args, double *) = strtod(text, NULL);
            else
                *va_arg(*args, float *) = strtof(text, NULL);
        }
    }
    if (item.field.heap != NULL)
        free(item.field.heap);
    return result;
}

/* Stage output so that a short %c or a wcrtomb failure cannot partially
   assign the destination. Suppression needs no storage. */
static enum scan_result wide_scan_encode(struct scan_field *field,
                                         wchar_t ch, mbstate_t *state,
                                         const struct scan_conversion *conversion)
{
    if (conversion->length == SCAN_L)
        return conversion->suppress ? SCAN_OK : scan_append(field, &ch, sizeof(ch));
    char bytes[MB_LEN_MAX];
    size_t size = wcrtomb(bytes, ch, state);
    if (size == (size_t)-1)
        return SCAN_INPUT_FAILURE;
    return conversion->suppress ? SCAN_OK : scan_append(field, bytes, size);
}

static enum scan_result scan_wide_characters(struct scan_input *input,
                                             const struct scan_conversion *conversion,
                                             va_list *args)
{
    int fixed = conversion->kind == 'c';
    size_t width = conversion->width != 0 ? conversion->width : fixed ? 1 : SIZE_MAX;
    size_t used = 0;
    mbstate_t state = {0};
    struct scan_field field = { .capacity = sizeof(field.local) };
    enum scan_result result = SCAN_OK;

    while (used < width) {
        wint_t ch = scan_peek(input);
        if (ch == WEOF || (conversion->kind == 's' && iswspace(ch)) ||
            (conversion->kind == '[' &&
             wide_set_contains(conversion, ch) == conversion->invert))
            break;
        scan_consume(input);
        ++used;
        if (result == SCAN_OK) {
            result = wide_scan_encode(&field, (wchar_t)ch, &state, conversion);
            if (result == SCAN_DIAGNOSTIC)
                break;
            /* Even on a wcrtomb error, finish reading the input item so
               the first wide character after the field is left unread. */
        }
    }

    if (result == SCAN_OK) {
        if (used == 0)
            result = input->next == WEOF ? SCAN_INPUT_FAILURE : SCAN_MATCH_FAILURE;
        else if (fixed && used != width)
            result = SCAN_MATCH_FAILURE;
        else if (!conversion->suppress) {
            if (!fixed)
                result = wide_scan_encode(&field, L'\0', &state, conversion);
            if (result == SCAN_OK) {
                void *out = conversion->length == SCAN_L
                                ? va_arg(*args, wchar_t *) : va_arg(*args, void *);
                memcpy(out, field.heap != NULL ? field.heap : field.local, field.used);
            }
        }
    }
    if (field.heap != NULL)
        free(field.heap);
    return result;
}

static int scan(struct scan_input *input, const void *format_text, va_list incoming)
{
    struct scan_format format = { .text = format_text, .wide = input->wide };
    va_list args;
    int assignments = 0;
    int completed = 0;
    enum scan_result result = SCAN_OK;
    va_copy(args, incoming);

    wint_t ch;
    while ((ch = scan_format_at(&format, 0)) != 0) {
        ++format.pos;
        if (scan_is_space(input->wide, ch)) {
            while (scan_is_space(input->wide, scan_format_at(&format, 0)))
                ++format.pos;
            scan_space(input);
        } else if (ch != '%') {
            result = scan_literal(input, ch);
        } else {
            struct scan_conversion conversion;
            result = scan_spec(&format, &conversion);
            if (result != SCAN_OK)
                break;
            ch = conversion.kind;
            if (ch != '[' && ch != 'c' && ch != 'n')
                scan_space(input);
            if (ch == '%') {
                result = scan_literal(input, '%');
            } else if (ch == 'n') {
                result = store_signed(&args, conversion.length,
                                      input->consumed, 0);
            } else if (ch == '[' || ch == 'c' || ch == 's') {
                result = input->wide ? scan_wide_characters(input, &conversion, &args)
                                     : scan_characters(input, &conversion, &args);
            } else if (ch == 'a' || ch == 'A' || ch == 'e' || ch == 'E' ||
                       ch == 'f' || ch == 'F' || ch == 'g' || ch == 'G') {
                result = scan_float(input, &conversion, &args);
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

    /* No seeking is needed, including on pipes. One pushback slot suffices
       and any pushback originally in the stream was read by scan_peek. */
    if (input->stream != NULL && input->buffered && input->next != WEOF) {
        if (input->wide)
            (void)ungetwc(input->next, input->stream);
        else
            (void)ungetc((int)input->next, input->stream);
    }
    if ((result == SCAN_INPUT_FAILURE && !completed) ||
        (result == SCAN_DIAGNOSTIC && assignments == 0))
        return EOF;
    return assignments;
}

int vfscanf(FILE *stream, const char *format, va_list args)
{
    struct scan_input input = { .stream = stream };
    if (stream == NULL || format == NULL) {
        errno = EINVAL;
        return EOF;
    }
    if (__cephyr_stdio_orient(stream, CEPHYR_STREAM_BYTE) == EOF)
        return EOF;
    return scan(&input, format, args);
}

int vscanf(const char *format, va_list args)
{
    return vfscanf(stdin, format, args);
}

int vsscanf(const char *text, const char *format, va_list args)
{
    struct scan_input input = { .text = text };
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

int vfwscanf(FILE *stream, const wchar_t *format, va_list args)
{
    struct scan_input input = { .stream = stream, .wide = 1 };
    if (stream == NULL || format == NULL) {
        errno = EINVAL;
        return EOF;
    }
    if (__cephyr_stdio_orient(stream, CEPHYR_STREAM_WIDE) == EOF)
        return EOF;
    return scan(&input, format, args);
}

int vwscanf(const wchar_t *format, va_list args)
{
    return vfwscanf(stdin, format, args);
}

int vswscanf(const wchar_t *text, const wchar_t *format, va_list args)
{
    struct scan_input input = { .text = text, .wide = 1 };
    if (text == NULL || format == NULL) {
        errno = EINVAL;
        return EOF;
    }
    return scan(&input, format, args);
}

int fwscanf(FILE *stream, const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfwscanf(stream, format, args);
    va_end(args);
    return result;
}

int wscanf(const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vwscanf(format, args);
    va_end(args);
    return result;
}

int swscanf(const wchar_t *text, const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vswscanf(text, format, args);
    va_end(args);
    return result;
}
