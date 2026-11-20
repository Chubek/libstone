#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wctype.h>

static void wide_classification(void)
{
    static const struct {
        const char *name;
        int (*narrow)(int);
        int (*wide)(wint_t);
    } classes[] = {
        {"alnum", isalnum, iswalnum}, {"alpha", isalpha, iswalpha},
        {"blank", isblank, iswblank}, {"cntrl", iscntrl, iswcntrl},
        {"digit", isdigit, iswdigit}, {"graph", isgraph, iswgraph},
        {"lower", islower, iswlower}, {"print", isprint, iswprint},
        {"punct", ispunct, iswpunct}, {"space", isspace, iswspace},
        {"upper", isupper, iswupper}, {"xdigit", isxdigit, iswxdigit},
    };
    wctrans_t lower = wctrans("tolower"), upper = wctrans("toupper");

    assert(!wctype("unknown") && !iswctype('a', 0));
    assert(!wctrans("unknown") && lower && upper);
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        wctype_t desc = wctype(classes[i].name);
        assert(desc && !classes[i].wide(WEOF) && !iswctype(WEOF, desc));
        assert(!iswctype(0x10041, desc)); /* Not ASCII 'A'. */
        for (int c = 0; c < 128; ++c) {
            assert(!!classes[i].narrow(c) == !!classes[i].wide((wint_t)c));
            assert(!!iswctype((wint_t)c, desc) == !!classes[i].wide((wint_t)c));
        }
    }
    for (int c = 0; c < 128; ++c) {
        assert(towctrans((wint_t)c, 0) == (wint_t)c);
        assert(towctrans((wint_t)c, lower) == towlower((wint_t)c));
        assert(towctrans((wint_t)c, upper) == towupper((wint_t)c));
        assert(towlower((wint_t)c) == (wint_t)tolower(c));
        assert(towupper((wint_t)c) == (wint_t)toupper(c));
    }
    assert(towctrans(WEOF, lower) == WEOF && towctrans(WEOF, upper) == WEOF);
    assert(towlower(0x10041) == 0x10041 && towupper(0x10061) == 0x10061);
}

static void locale_and_conversion(void)
{
    struct lconv *locale = localeconv();
    wchar_t wide[] = {'X', 'X', 'X', 'X'};
    const wchar_t invalid[] = {0x80, 0};
    char bytes[] = "XXX";

    assert(strcmp(setlocale(LC_ALL, NULL), "C") == 0);
    assert(strcmp(setlocale(LC_CTYPE, ""), "C") == 0);
    assert(strcmp(setlocale(LC_NUMERIC, "POSIX"), "C") == 0);
    assert(setlocale(LC_ALL, "C.UTF-8") == NULL && setlocale(-1, "C") == NULL);
    assert(strcmp(setlocale(LC_ALL, NULL), "C") == 0);
    assert(strcmp(locale->decimal_point, ".") == 0 && *locale->grouping == '\0');
    assert(*locale->int_curr_symbol == '\0' && locale->int_p_sign_posn == CHAR_MAX);
    assert(locale->frac_digits == CHAR_MAX && MB_CUR_MAX == 1);

    assert(mblen(NULL, 0) == 0 && mblen("", 1) == 0 && mblen("A", 1) == 1);
    assert(mblen("A", 0) == -1 && errno == EILSEQ);
    assert(mbtowc(wide, "A", 1) == 1 && wide[0] == 'A');
    assert(mbtowc(wide, "\x80", 1) == -1 && wide[0] == 'A');
    assert(wctomb(NULL, 0) == 0 && wctomb(bytes, 0) == 1 && bytes[0] == '\0');
    assert(wctomb(bytes, (wchar_t)-1) == -1 && errno == EILSEQ);
    assert(mbstowcs(wide, "ab\x80", 2) == 2 && wide[2] == 'X');
    assert(mbstowcs(wide, "ab", 4) == 2 && wide[2] == 0 && wide[3] == 'X');
    assert(mbstowcs(NULL, "test", 0) == 4);
    assert(wcstombs(bytes, wide, 1) == 1 && bytes[1] == 'X');
    assert(wcstombs(bytes, wide, sizeof(bytes)) == 2 && strcmp(bytes, "ab") == 0);
    assert(wcstombs(NULL, wide, 0) == 2 && wcstombs(bytes, invalid, 0) == 0);
    assert(wcstombs(NULL, invalid, 0) == (size_t)-1 && errno == EILSEQ);
}

static void allocation_and_environment(void)
{
    size_t alignment;
    char *sample = getenv("CEPHYR_STDLIB_TEST");
    char *empty = getenv("CEPHYR_STDLIB_TEST_EMPTY");
    assert(sample != NULL && strcmp(sample, "value=1") == 0);
    assert(empty != NULL && *empty == '\0');
    assert(getenv("") == NULL && getenv("CEPHYR_STDLIB_TEST=") == NULL);
    assert(getenv("CEPHYR_STDLIB_TEST_ABSENT") == NULL);
    for (alignment = 1; alignment <= 65536; alignment *= 2) {
        unsigned char *data = aligned_alloc(alignment, alignment * 2);
        unsigned char *resized;
        assert(data != NULL && (uintptr_t)data % alignment == 0);
        data[0] = 42;
        data[alignment * 2 - 1] = 23;
        resized = realloc(data, alignment * 2 + 1);
        assert(resized != NULL && resized[0] == 42 &&
               resized[alignment * 2 - 1] == 23);
        assert((uintptr_t)resized % _Alignof(max_align_t) == 0);
        free(resized);
    }
    assert(aligned_alloc(3, 6) == NULL && errno == EINVAL);
    assert(aligned_alloc(16, 17) == NULL && errno == EINVAL);
    assert(aligned_alloc(16, SIZE_MAX - 15) == NULL && errno == ENOMEM);
    free(aligned_alloc(16, 0));
}

static void normal_handler(void)
{
    (void)putchar('N');
}

static void quick_first(void)
{
    (void)write(STDOUT_FILENO, "1", 1);
}

static void quick_second(void)
{
    (void)write(STDOUT_FILENO, "2", 1);
}

int main(int argc, char **argv)
{
    assert(atexit(normal_handler) == 0);
    assert(at_quick_exit(quick_first) == 0);
    assert(at_quick_exit(quick_second) == 0);
    assert(setvbuf(stdout, NULL, _IOFBF, 0) == 0);
    assert(putchar('B') == 'B');
    if (argc == 2 && strcmp(argv[1], "quick") == 0)
        quick_exit(17);
    if (argc == 2 && strcmp(argv[1], "immediate") == 0)
        _Exit(23);
    locale_and_conversion();
    wide_classification();
    allocation_and_environment();
    return 0;
}
