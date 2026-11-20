#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "internal.h"

#include <errno.h>
#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

mv_status mv_fail(mv_diagnostic *diag, mv_status status, mv_location loc, const char *message)
{
    if (diag) {
        diag->offset = loc.offset;
        diag->line = loc.line;
        diag->column = loc.column;
        diag->message = message;
    }
    return status;
}

int mv_slice_eq(mv_slice slice, const char *literal)
{
    return slice.size == strlen(literal) &&
           (slice.size == 0 || memcmp(slice.data, literal, slice.size) == 0);
}

#ifdef _WIN32
typedef _locale_t mv_numeric_locale;

static mv_numeric_locale open_numeric_locale(void)
{
    return _create_locale(LC_NUMERIC, "C");
}

static void close_numeric_locale(mv_numeric_locale locale)
{
    _free_locale(locale);
}
#else
typedef locale_t mv_numeric_locale;

static mv_numeric_locale open_numeric_locale(void)
{
    return newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}

static void close_numeric_locale(mv_numeric_locale locale)
{
    freelocale(locale);
}
#endif

mv_status mv_parse_number(mv_slice token, double *result)
{
    char small[128];
    char *text = small;
    char *end;
    mv_numeric_locale locale;
    mv_status status = MV_STATUS_OK;
    int range_error, consumed;
    double value;
#ifndef _WIN32
    locale_t previous;
#endif

    if (token.size == SIZE_MAX)
        return MV_STATUS_LIMIT_EXCEEDED;
    if (token.size >= sizeof small) {
        text = malloc(token.size + 1);
        if (!text)
            return MV_STATUS_OUT_OF_MEMORY;
    }
    memcpy(text, token.data, token.size);
    text[token.size] = '\0';
    locale = open_numeric_locale();
    if (!locale) {
        status = MV_STATUS_OUT_OF_MEMORY;
        goto done;
    }
#ifdef _WIN32
    errno = 0;
    value = _strtod_l(text, &end, locale);
    range_error = errno == ERANGE;
#else
    previous = uselocale(locale);
    if (!previous) {
        close_numeric_locale(locale);
        status = MV_STATUS_OUT_OF_MEMORY;
        goto done;
    }
    errno = 0;
    value = strtod(text, &end);
    range_error = errno == ERANGE;
    uselocale(previous);
#endif
    close_numeric_locale(locale);
    consumed = end != text && end == text + token.size;

    if (!consumed || !isfinite(value) || (range_error && fabs(value) >= DBL_MAX)) {
        status = MV_STATUS_SYNTAX_ERROR;
        goto done;
    }
    if (value == 0) {
        size_t i;
        /* Subnormals are representable. Reject only nonzero literals that vanished. */
        for (i = 0; i < token.size && text[i] != 'e' && text[i] != 'E'; ++i) {
            if (text[i] >= '1' && text[i] <= '9') {
                status = MV_STATUS_SYNTAX_ERROR;
                goto done;
            }
        }
    }
    *result = value;

done:
    if (text != small)
        free(text);
    return status;
}

int mv_format_number(char *buffer, size_t capacity, double number)
{
    mv_numeric_locale locale = open_numeric_locale();
    int length;
#ifndef _WIN32
    locale_t previous;
#endif

    if (!locale)
        return -1;
#ifdef _WIN32
    length = _scprintf_l("%.*g", locale, DBL_DECIMAL_DIG, number);
    if (length >= 0 && (size_t)length < capacity) {
        if (_snprintf_l(buffer, capacity, "%.*g", locale, DBL_DECIMAL_DIG, number) < 0)
            length = -1;
    } else if (capacity) {
        _snprintf_l(buffer, capacity, "%.*g", locale, DBL_DECIMAL_DIG, number);
        buffer[capacity - 1] = '\0';
    }
#else
    previous = uselocale(locale);
    if (!previous) {
        close_numeric_locale(locale);
        return -1;
    }
    length = snprintf(buffer, capacity, "%.*g", DBL_DECIMAL_DIG, number);
    uselocale(previous);
#endif
    close_numeric_locale(locale);
    return length;
}
