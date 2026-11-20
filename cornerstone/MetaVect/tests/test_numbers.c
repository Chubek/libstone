#include "internal.h"

#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do {                                                    \
    if (!(expr)) {                                                          \
        fprintf(stderr, "line %d: check failed: %s\n", __LINE__, #expr);     \
        return 1;                                                          \
    }                                                                      \
} while (0)

static mv_status parse(const char *text, double *value)
{
    return mv_parse_number((mv_slice){text, strlen(text)}, value);
}

int main(void)
{
    const double numbers[] = {-0.0, 0.0, 1.25, 1.0 / 3.0, -DBL_MAX, DBL_MAX, DBL_TRUE_MIN};
    const char *foreign_locales[] = {"de_DE.UTF-8", "fr_FR.UTF-8", "German_Germany.1252"};
    double value;
    char text[128], long_text[260];
    size_t i;
    int foreign_locale = 0;

    /* Exercise a comma decimal separator when the host has one installed. */
    for (i = 0; i < sizeof foreign_locales / sizeof *foreign_locales; ++i) {
        if (setlocale(LC_NUMERIC, foreign_locales[i])) {
            foreign_locale = 1;
            break;
        }
    }
    for (i = 0; i < sizeof numbers / sizeof *numbers; ++i) {
        int n = mv_format_number(text, sizeof text, numbers[i]);
        CHECK(n > 0 && (size_t)n == strlen(text));
        CHECK(strchr(text, ',') == NULL);
        CHECK(parse(text, &value) == MV_STATUS_OK);
        CHECK(value == numbers[i] && !!signbit(value) == !!signbit(numbers[i]));
    }
    CHECK(mv_format_number(NULL, 0, 1.25) == 4);
    CHECK(mv_format_number(text, 3, 1.25) == 4 && strcmp(text, "1.") == 0);
    CHECK(parse("1e9999", &value) == MV_STATUS_SYNTAX_ERROR);
    CHECK(parse("-1e-9999", &value) == MV_STATUS_SYNTAX_ERROR);
    CHECK(parse("-0.000e-9999", &value) == MV_STATUS_OK && value == 0 && signbit(value));
    memset(long_text, '0', sizeof long_text);
    long_text[sizeof long_text - 2] = '9';
    long_text[sizeof long_text - 1] = '\0';
    CHECK(parse(long_text, &value) == MV_STATUS_OK && value == 9);
    if (foreign_locale)
        CHECK(strcmp(localeconv()->decimal_point, ",") == 0);
    return 0;
}
