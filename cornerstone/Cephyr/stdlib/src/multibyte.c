#include <errno.h>
#include <stdio.h>
#include <uchar.h>
#include <wchar.h>

/* All representable characters are ASCII. Decoding never leaves the
   initial conversion state, even when no input bytes are supplied. */
static size_t decode(unsigned int *out, const char *src, size_t n)
{
    if (n == 0)
        return (size_t)-2;
    unsigned char byte = (unsigned char)*src;
    if (byte > 0x7f) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    *out = byte;
    return byte != 0;
}

static size_t encode(char *out, unsigned int wc)
{
    if (wc > 0x7f) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (out != NULL)
        *out = (char)wc;
    return 1;
}

wint_t btowc(int c)
{
    return c == EOF || (unsigned char)c > 0x7f ? WEOF : (unsigned char)c;
}

int wctob(wint_t wc)
{
    return wc <= 0x7f ? (int)wc : EOF;
}

int mbsinit(const mbstate_t *ps)
{
    return ps == NULL || *ps == 0;
}

size_t mbrtowc(wchar_t *out, const char *src, size_t n, mbstate_t *ps)
{
    (void)ps;
    if (src == NULL)
        return 0;
    unsigned int wc;
    size_t count = decode(&wc, src, n);
    if (count <= 1 && out != NULL)
        *out = (wchar_t)wc;
    return count;
}

size_t mbrlen(const char *src, size_t n, mbstate_t *ps)
{
    return mbrtowc(NULL, src, n, ps);
}

size_t wcrtomb(char *out, wchar_t wc, mbstate_t *ps)
{
    (void)ps;
    return out == NULL ? 1 : encode(out, (unsigned int)wc);
}

size_t mbrtoc16(char16_t *out, const char *src, size_t n, mbstate_t *ps)
{
    (void)ps;
    if (src == NULL)
        return 0;
    unsigned int wc;
    size_t count = decode(&wc, src, n);
    if (count <= 1 && out != NULL)
        *out = (char16_t)wc;
    return count;
}

size_t c16rtomb(char *out, char16_t wc, mbstate_t *ps)
{
    static mbstate_t internal;
    if (ps == NULL)
        ps = &internal;
    if (out == NULL)
        wc = 0;
    if (*ps != 0) {
        /* A trailing surrogate would complete a non-ASCII character; any
           other code unit would leave a malformed pair. Neither encodes. */
        *ps = 0;
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (wc >= 0xd800 && wc <= 0xdbff) {
        *ps = 1;
        return 0;
    }
    return encode(out, wc);
}

size_t mbrtoc32(char32_t *out, const char *src, size_t n, mbstate_t *ps)
{
    (void)ps;
    if (src == NULL)
        return 0;
    unsigned int wc;
    size_t count = decode(&wc, src, n);
    if (count <= 1 && out != NULL)
        *out = (char32_t)wc;
    return count;
}

size_t c32rtomb(char *out, char32_t wc, mbstate_t *ps)
{
    (void)ps;
    return out == NULL ? 1 : encode(out, wc);
}

size_t mbsrtowcs(wchar_t *out, const char **src, size_t len, mbstate_t *ps)
{
    (void)ps;
    const char *input = *src;
    for (size_t i = 0; out == NULL || i < len; ++i) {
        unsigned int wc;
        size_t count = decode(&wc, input + i, 1);
        if (count == (size_t)-1)
            return count;
        if (out != NULL) {
            out[i] = (wchar_t)wc;
            *src = count == 0 ? NULL : input + i + 1;
        }
        if (count == 0)
            return i;
    }
    return len;
}

size_t wcsrtombs(char *out, const wchar_t **src, size_t len, mbstate_t *ps)
{
    (void)ps;
    const wchar_t *input = *src;
    for (size_t i = 0; out == NULL || i < len; ++i) {
        wchar_t wc = input[i];
        if (encode(out == NULL ? NULL : out + i, (unsigned int)wc) == (size_t)-1)
            return (size_t)-1;
        if (out != NULL)
            *src = wc == 0 ? NULL : input + i + 1;
        if (wc == 0)
            return i;
    }
    return len;
}
