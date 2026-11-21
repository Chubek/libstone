#include <stddef.h>
#include <uchar.h>

#define HAS_TYPE(expression, type) _Generic((expression), type: 1, default: 0)
_Static_assert(HAS_TYPE('a', int), "ordinary type");
_Static_assert(HAS_TYPE(L'a', wchar_t), "wchar_t type");
_Static_assert(HAS_TYPE(u'a', char16_t), "char16_t type");
_Static_assert(HAS_TYPE(U'a', char32_t), "char32_t type");
_Static_assert(sizeof(u'a') == 2, "char16_t is not promoted in sizeof");
_Static_assert(sizeof(U'a') == 4, "char32_t width");

_Static_assert('\xff' == (char)255, "single-byte target char signedness");
_Static_assert('\0\xff' == 255, "two bytes with a leading zero");
_Static_assert('abcde' == 0x62636465, "last four bytes are packed");
_Static_assert('é' == '\u00e9' && 'é' == 0xc3a9, "UTF-8 byte packing");
_Static_assert(L'\xFFFFFFFF' == (wchar_t)-1, "32-bit numeric escape");
_Static_assert(u'\U0001f642' == 0xde42, "non-BMP UTF-16 low surrogate");
_Static_assert(U'🙂' == 0x1f642, "UTF-32 value");
_Static_assert(L'ab' == L'b', "last wide code unit");
_Static_assert((1 ? -1 : U'a') > 0, "ICE conditional uses unsigned type");
_Static_assert((1 ? -1 : u'a') < 0, "ICE conditional promotes char16_t");

enum { rocket = U'\U0001f680' };
static char32_t codepoint = U'\U0001f680';
static wchar_t wide = L'\xFFFFFFFF';

int main(void) {
    char16_t letter = u'\u00e9';
    switch (codepoint) {
    case U'\U0001f680':
        return rocket != codepoint || wide != (wchar_t)-1 || letter != 0xe9;
    default:
        return 1;
    }
}
