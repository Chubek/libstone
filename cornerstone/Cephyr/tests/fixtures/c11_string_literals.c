#include <stddef.h>
#include <stdint.h>
#include <uchar.h>

#define HAS_TYPE(expression, type) _Generic((expression), type: 1, default: 0)
#define UTF16 u"\U0001f642"
#define UTF32 U"\U0001f642"

_Static_assert(HAS_TYPE(L"", wchar_t *), "L array decay");
_Static_assert(HAS_TYPE(u"", char16_t *), "u array decay");
_Static_assert(HAS_TYPE(U"", char32_t *), "U array decay");
_Static_assert(HAS_TYPE(u8"", char *), "u8 array decay");
_Static_assert(HAS_TYPE(&("a" u"b"), char16_t (*)[3]), "address of UTF-16 array");
_Static_assert(HAS_TYPE(&U"", char32_t (*)[1]), "address of UTF-32 array");
_Static_assert(sizeof(L"" "") == sizeof(wchar_t), L"one terminator");
_Static_assert(sizeof(UTF16 "Z") == 4 * sizeof(char16_t), "surrogate pair and terminator");
_Static_assert(sizeof(UTF32 "Z") == 3 * sizeof(char32_t), "one unit for non-BMP scalar");
_Static_assert(sizeof(u8"🙂" "é") == 7, "UTF-8 character array");

static const wchar_t wide[] = {L"\x12" /* escapes are token-local */ L"3" "\0é" L"\777"};
static const char16_t utf16[] = "é" UTF16 "\xFF" u"\xD800\xFFFF";
static char32_t utf32[7] = {"\u00e9" UTF32 U"\xFFFFFFFF" "\0A"};
static const unsigned char utf8[] = {u8"é" "\0" u8"\U0001f642"};
static const wchar_t *wide_address = L"\xFFFFFFFF";
static const char16_t *interior = &u"a" u"b"[1];
static const char16_t *commuted = 1 + u"ab";
static const char32_t *offset_address = U"ab" - (-1);
static const char32_t *one_past = U"ab" + 3;
static char32_t (*whole)[2] = &U"\xFFFFFFFF";
static char32_t *pointer_array[] = {U"q", U"r"};
static unsigned char low_align[] = "X\0";
static const char *bytes_address = "X\0"; // Same bytes as u"X".
static const char16_t *utf16_address = u"X";

struct strings {
    char16_t first[2];
    char32_t second[3];
};
static struct strings nested = {UTF16, U"\x3ff"};
static char16_t matrix[][3] = {u"1", u"2"};

_Static_assert(sizeof(wide) == 6 * sizeof(wchar_t), "static inferred count");
_Static_assert(sizeof(utf16) == 7 * sizeof(char16_t), "infer by units, not bytes");
_Static_assert(sizeof(matrix) == 6 * sizeof(char16_t), "nested initializer count");

static const char16_t *from_function(void) {
    return u"x" "y";
}

int main(void) {
    char16_t local[] = {u"\x1234" "\0" u"🙂"};
    wchar_t exact[1] = L"\xFFFFFFFF";
    static char16_t local_static[] = u"\u00a3";
    char32_t local_nested[2][3] = {U"\x12345678", U"\0\U0001f642"};
    volatile char16_t volatile_local[4] = u"\x1234";
    char32_t (*address)[2] = &U"Z";

    if (wide[0] != 0x12 || wide[1] != '3' || wide[2] ||
        wide[3] != 0xe9 || wide[4] != 0777 || wide[5])
        return 1;
    if (utf16[0] != 0xe9 || utf16[1] != 0xd83d || utf16[2] != 0xde42 ||
        utf16[3] != 0xff || utf16[4] != 0xd800 || utf16[5] != 0xffff || utf16[6])
        return 2;
    if (utf32[0] != 0xe9 || utf32[1] != 0x1f642 || utf32[2] != 0xffffffffu ||
        utf32[3] || utf32[4] != 'A' || utf32[5] || utf32[6])
        return 3;
    if (utf8[0] != 0xc3 || utf8[1] != 0xa9 || utf8[2] || utf8[3] != 0xf0 ||
        utf8[4] != 0x9f || utf8[5] != 0x99 || utf8[6] != 0x82 || utf8[7])
        return 4;
    if (*wide_address != (wchar_t)-1 || *interior != 'b' ||
        (*whole)[0] != 0xffffffffu || (*whole)[1] || (*address)[0] != 'Z' ||
        *pointer_array[0] != 'q' || *pointer_array[1] != 'r' ||
        *commuted != 'b' || *offset_address != 'b' || *(one_past - 2) != 'b')
        return 5;
    if ((uintptr_t)wide_address % _Alignof(wchar_t) ||
        (uintptr_t)utf16_address % _Alignof(char16_t) ||
        (uintptr_t)whole % _Alignof(char32_t) || *utf16_address != 'X' ||
        bytes_address[0] != 'X' || low_align[0] != 'X')
        return 6;
    if (nested.first[0] != 0xd83d || nested.first[1] != 0xde42 ||
        nested.second[0] != 0x3ff || nested.second[1] || nested.second[2] ||
        matrix[0][0] != '1' || matrix[1][0] != '2')
        return 7;
    if (sizeof(local) != 5 * sizeof(char16_t) || local[0] != 0x1234 || local[1] ||
        local[2] != 0xd83d || local[3] != 0xde42 || local[4] ||
        exact[0] != (wchar_t)-1 || local_static[0] != 0xa3 || local_static[1])
        return 8;
    if (local_nested[0][0] != 0x12345678 || local_nested[0][1] ||
        local_nested[1][0] || local_nested[1][1] != 0x1f642 || local_nested[1][2] ||
        volatile_local[0] != 0x1234 || volatile_local[1] || volatile_local[3])
        return 9;
    if (from_function()[1] != 'y' || L"🙂"[0] != 0x1f642 || u8"A" u8"B"[1] != 'B')
        return 10;
    return 0;
}
