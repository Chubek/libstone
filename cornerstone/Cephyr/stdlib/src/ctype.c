#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <wctype.h>

int isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}

int islower(int c)
{
    return c >= 'a' && c <= 'z';
}

int isalpha(int c)
{
    return isupper(c) || islower(c);
}

int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

int isspace(int c)
{
    return c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' ||
           c == '\v';
}

int isblank(int c)
{
    return c == ' ' || c == '\t';
}

int iscntrl(int c)
{
    return (c >= 0 && c < 0x20) || c == 0x7f;
}

int isprint(int c)
{
    return c >= 0x20 && c <= 0x7e;
}

int isgraph(int c)
{
    return c >= 0x21 && c <= 0x7e;
}

int ispunct(int c)
{
    return isgraph(c) && !isalnum(c);
}

int tolower(int c)
{
    return isupper(c) ? c + ('a' - 'A') : c;
}

int toupper(int c)
{
    return islower(c) ? c - ('a' - 'A') : c;
}

/* The bundled libc uses the C locale's ASCII repertoire. Test before
   narrowing; other wide values must not alias ASCII characters. */
#define WIDE_CLASSES(X) \
    X(alnum) X(alpha) X(blank) X(cntrl) X(digit) X(graph) \
    X(lower) X(print) X(punct) X(space) X(upper) X(xdigit)

#define WIDE_CLASS_FUNCTION(name)            \
    int isw##name(wint_t c)                  \
    {                                      \
        return c <= 0x7f && is##name((int)c); \
    }
WIDE_CLASSES(WIDE_CLASS_FUNCTION)
#undef WIDE_CLASS_FUNCTION

static const struct {
    const char *name;
    int (*test)(wint_t);
} wide_classes[] = {
#define WIDE_CLASS_ENTRY(name) {#name, isw##name},
    WIDE_CLASSES(WIDE_CLASS_ENTRY)
#undef WIDE_CLASS_ENTRY
};
#undef WIDE_CLASSES

wctype_t wctype(const char *property)
{
    for (size_t i = 0; i < sizeof(wide_classes) / sizeof(wide_classes[0]); ++i) {
        if (strcmp(wide_classes[i].name, property) == 0)
            return i + 1;
    }
    return 0;
}

int iswctype(wint_t c, wctype_t desc)
{
    return desc != 0 && desc <= sizeof(wide_classes) / sizeof(wide_classes[0])
        && wide_classes[desc - 1].test(c);
}

wint_t towlower(wint_t c)
{
    return c <= 0x7f ? (wint_t)tolower((int)c) : c;
}

wint_t towupper(wint_t c)
{
    return c <= 0x7f ? (wint_t)toupper((int)c) : c;
}

static const int map_lower = 1;
static const int map_upper = 2;

wctrans_t wctrans(const char *property)
{
    if (strcmp(property, "tolower") == 0)
        return &map_lower;
    if (strcmp(property, "toupper") == 0)
        return &map_upper;
    return NULL;
}

wint_t towctrans(wint_t c, wctrans_t desc)
{
    if (desc == &map_lower)
        return towlower(c);
    if (desc == &map_upper)
        return towupper(c);
    return c;
}
