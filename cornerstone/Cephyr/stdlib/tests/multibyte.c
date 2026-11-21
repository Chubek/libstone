#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <uchar.h>
#include <wchar.h>
#include <wctype.h>

static void characters(void)
{
    mbstate_t state = {0};
    assert(mbsinit(NULL) && mbsinit(&state));
    assert(btowc(EOF) == WEOF && wctob(WEOF) == EOF);
    assert(btowc(0x141) == 'A'); /* btowc examines (unsigned char)c. */
    for (unsigned i = 0; i < 128; ++i) {
        char src = (char)i, out = 0;
        wchar_t wc = 0;
        char16_t c16 = 0;
        char32_t c32 = 0;
        size_t count = i != 0;
        assert(btowc((int)i) == i && wctob(i) == (int)i);
        assert(mbrlen(&src, 1, &state) == count);
        assert(mbrtowc(&wc, &src, 1, &state) == count && wc == (wchar_t)i);
        assert(mbrtoc16(&c16, &src, 1, &state) == count && c16 == i);
        assert(mbrtoc32(&c32, &src, 1, &state) == count && c32 == i);
        assert(wcrtomb(&out, wc, &state) == 1 && out == src);
        assert(c16rtomb(&out, c16, &state) == 1 && out == src);
        assert(c32rtomb(&out, c32, &state) == 1 && out == src);
        assert(mbsinit(&state));
    }

    wchar_t wc = 'x';
    char16_t c16 = 'y';
    char32_t c32 = 'z';
    char out = 'X';
    assert(mbrtowc(&wc, "A", 0, &state) == (size_t)-2 && wc == 'x');
    assert(mbrtoc16(&c16, "A", 0, &state) == (size_t)-2 && c16 == 'y');
    assert(mbrtoc32(&c32, "A", 0, &state) == (size_t)-2 && c32 == 'z');
    assert(mbsinit(&state));
    assert(mbrtowc(&wc, NULL, 0, NULL) == 0 && wc == 'x');
    assert(mbrtoc16(&c16, NULL, 0, NULL) == 0 && c16 == 'y');
    assert(mbrtoc32(&c32, NULL, 0, NULL) == 0 && c32 == 'z');
    assert(wcrtomb(NULL, 0x100, NULL) == 1);
    assert(c16rtomb(NULL, 0x100, NULL) == 1);
    assert(c32rtomb(NULL, 0x100, NULL) == 1);

    for (int i = 128; i < 256; ++i)
        assert(btowc(i) == WEOF && wctob((wint_t)i) == EOF);
    errno = 0;
    assert(mbrtowc(&wc, "\xff", 1, NULL) == (size_t)-1 && wc == 'x' && errno == EILSEQ);
    errno = 0;
    assert(mbrtoc16(&c16, "\xc3\xa9", 2, NULL) == (size_t)-1 && c16 == 'y' && errno == EILSEQ);
    errno = 0;
    assert(mbrtoc32(&c32, "\x80", 1, NULL) == (size_t)-1 && c32 == 'z' && errno == EILSEQ);
    errno = 0;
    assert(wcrtomb(&out, (wchar_t)-1, NULL) == (size_t)-1 && out == 'X' && errno == EILSEQ);
    errno = 0;
    assert(c16rtomb(&out, 0xe9, NULL) == (size_t)-1 && out == 'X' && errno == EILSEQ);
    errno = 0;
    assert(c32rtomb(&out, 0x1f642, NULL) == (size_t)-1 && out == 'X' && errno == EILSEQ);

    /* UTF-16 can hold a leading surrogate pending a second code unit.
       Even a valid pair cannot be encoded in the ASCII locale. */
    errno = 0;
    assert(c16rtomb(&out, 0xd83d, &state) == 0 && !mbsinit(&state) && out == 'X');
    assert(c16rtomb(&out, 0xde42, &state) == (size_t)-1 && out == 'X' && errno == EILSEQ);
    state = (mbstate_t){0};
    assert(c16rtomb(&out, 0xdc00, &state) == (size_t)-1);
    assert(c16rtomb(&out, 0xd800, NULL) == 0);
    assert(c16rtomb(NULL, 0, NULL) == (size_t)-1); /* NUL cannot complete a pair. */
    assert(c16rtomb(&out, 'A', NULL) == 1 && out == 'A');
}

static void strings(void)
{
    mbstate_t state = {0};
    const char *ascii = "abc", *mb = ascii;
    wchar_t wide[4] = { 'X', 'X', 'X', 'X' };
    assert(mbsrtowcs(NULL, &mb, 0, &state) == 3 && mb == ascii);
    assert(mbsrtowcs(wide, &mb, 0, &state) == 0 && mb == ascii && wide[0] == 'X');
    assert(mbsrtowcs(wide, &mb, 2, &state) == 2 && mb == ascii + 2);
    assert(wide[0] == 'a' && wide[1] == 'b' && wide[2] == 'X');
    assert(mbsrtowcs(wide + 2, &mb, 2, &state) == 1 && !mb);
    assert(wide[2] == 'c' && wide[3] == 0 && mbsinit(&state));
    const char *invalid = "a\x80";
    mb = invalid;
    errno = 0;
    assert(mbsrtowcs(wide, &mb, 4, NULL) == (size_t)-1 && errno == EILSEQ);
    assert(mb == invalid + 1 && wide[0] == 'a');
    mb = invalid;
    assert(mbsrtowcs(NULL, &mb, 4, NULL) == (size_t)-1 && mb == invalid);

    const wchar_t *wc = wide;
    char bytes[] = "XXXX";
    assert(wcsrtombs(NULL, &wc, 0, &state) == 3 && wc == wide);
    assert(wcsrtombs(bytes, &wc, 0, &state) == 0 && wc == wide && bytes[0] == 'X');
    assert(wcsrtombs(bytes, &wc, 3, &state) == 3 && wc == wide + 3 && bytes[3] == 'X');
    assert(wcsrtombs(bytes + 3, &wc, 1, &state) == 0 && !wc);
    assert(bytes[0] == 'a' && bytes[1] == 'b' && bytes[2] == 'c' && bytes[3] == '\0');
    assert(mbsinit(&state));
    const wchar_t bad[] = { 'a', (wchar_t)0x80, 0 };
    wc = bad;
    errno = 0;
    assert(wcsrtombs(bytes, &wc, 4, NULL) == (size_t)-1 && errno == EILSEQ);
    assert(wc == bad + 1 && bytes[0] == 'a');
    wc = bad;
    assert(wcsrtombs(NULL, &wc, 4, NULL) == (size_t)-1 && wc == bad);
}

int main(void)
{
    characters();
    strings();
    return 0;
}
