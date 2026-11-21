#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <wchar.h>

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

static int check_integers(void)
{
    const wchar_t *input;
    wchar_t *end;
    const wchar_t high_value[] = {L'1', 0x132, L'3', 0};

    input = L" \t-0x28!";
    errno = EDOM;
    CHECK(wcstol(input, &end, 0) == -40 && *end == L'!' && errno == EDOM);
    CHECK(wcstoll(L"012", &end, 0) == 10 && *end == 0);
    CHECK(wcstoll(L"0X12", &end, 16) == 18 && *end == 0);
    CHECK(wcstoul(L"Z", NULL, 36) == 35);
    CHECK(wcstoull(L"-1", &end, 10) == ULLONG_MAX && *end == 0);
    CHECK(wcstoul(L"-18446744073709551615", &end, 10) == 1UL && *end == 0);
    CHECK(wcstoimax(L"-9223372036854775808", &end, 10) == INTMAX_MIN &&
          *end == 0 && errno == EDOM);
    CHECK(wcstoumax(L"18446744073709551615", &end, 10) == UINTMAX_MAX &&
          *end == 0 && errno == EDOM);

    input = L"  +z";
    CHECK(wcstol(input, &end, 10) == 0 && end == input && errno == EDOM);
    CHECK(wcstol(input, &end, 1) == 0 && end == input && errno == EINVAL);
    CHECK(wcstol(L"0x@", &end, 0) == 0 && *end == L'x');
    CHECK(wcstol(L"0b1", &end, 0) == 0 && *end == L'b'); /* Not a C11 prefix. */
    CHECK(wcstol(high_value, &end, 10) == 1 && end == high_value + 1);

    errno = 0;
    CHECK(wcstol(L"9223372036854775808abc", &end, 10) == LONG_MAX &&
          *end == L'a' && errno == ERANGE);
    errno = 0;
    CHECK(wcstoll(L"-9223372036854775809", &end, 10) == LLONG_MIN &&
          *end == 0 && errno == ERANGE);
    errno = 0;
    CHECK(wcstoimax(L"-ffffffffffffffffffffffffg", &end, 16) == INTMAX_MIN &&
          *end == L'g' && errno == ERANGE);
    errno = 0;
    CHECK(wcstoumax(L"-18446744073709551616!", &end, 10) == UINTMAX_MAX &&
          *end == L'!' && errno == ERANGE);
    return 0;
}

int main(void)
{
    wchar_t buffer[10];
    wchar_t a[] = {L'a', 0, 0x1234, L'z'};
    wchar_t b[] = {L'a', 0, 0x2345, L'z'};
    wchar_t alpha[] = L"  red,green.blue";
    wchar_t words[] = L"foo|bar";
    wchar_t *alpha_state;
    wchar_t *words_state;
    const wchar_t *str = L"abcab";

    CHECK(wcslen(L"") == 0 && wcslen(str) == 5);
    CHECK(wcscpy(buffer, str) == buffer && wcscmp(buffer, str) == 0);
    CHECK(wcscat(buffer, L"de") == buffer && wcscmp(buffer, L"abcabde") == 0);
    CHECK(wcsncat(buffer, L"fgj", 2) == buffer && wcscmp(buffer, L"abcabdefg") == 0);
    CHECK(wcsncpy(buffer, L"x", 5) == buffer && buffer[0] == L'x');
    CHECK(buffer[1] == 0 && buffer[2] == 0 && buffer[3] == 0 && buffer[4] == 0 &&
          buffer[5] == L'd');
    CHECK(wcsncpy(buffer, L"test", 2) == buffer && buffer[0] == L't' &&
          buffer[1] == L'e' && buffer[2] == 0);
    CHECK(wcsncpy(buffer, L"TEST", 0) == buffer && buffer[0] == L't');
    CHECK(wcsncmp(str, L"abDEF", 2) == 0 && wcsncmp(str, L"abDEF", 3) > 0);
    CHECK(wcsncmp(a, b, 4) == 0 && wmemcmp(a, b, 4) < 0);
    CHECK(wcscmp(L"a", L"ab") < 0 && wcscoll(L"y", L"z") < 0);
    CHECK(wcsxfrm(NULL, str, 0) == 5 && wcsxfrm(buffer, str, 10) == 5);
    CHECK(wcscmp(buffer, str) == 0);
    CHECK(wcschr(str, L'b') == str + 1 && wcsrchr(str, L'b') == str + 4);
    CHECK(wcschr(str, 0) == str + 5 && wcsrchr(str, 0) == str + 5);
    CHECK(wcschr(str, L'x') == NULL && wcsrchr(str, L'x') == NULL);
    CHECK(wcsspn(str, L"ab") == 2 && wcsspn(str, L"") == 0);
    CHECK(wcscspn(str, L"cb") == 1 && wcscspn(str, L"") == 5);
    CHECK(wcspbrk(str, L"cb") == str + 1 && wcspbrk(str, L"xy") == NULL);
    CHECK(wcsstr(str, L"cab") == str + 2 && wcsstr(str, L"") == str);
    CHECK(wcsstr(str, L"abcabc") == NULL && wcsstr(str, L"ba") == NULL);
    CHECK(wmemchr(a, 0x1234, 4) == a + 2 && wmemchr(a, 0x1234, 2) == NULL);
    CHECK(wmemcpy(buffer, b, 4) == buffer && wmemcmp(buffer, b, 4) == 0);
    CHECK(wmemmove(buffer + 1, buffer, 4) == buffer + 1 &&
          wmemcmp(buffer + 1, b, 4) == 0);
    CHECK(wmemmove(buffer, buffer + 1, 4) == buffer && wmemcmp(buffer, b, 4) == 0);
    CHECK(wmemset(buffer, 0x1234, 5) == buffer && buffer[0] == 0x1234 &&
          buffer[4] == 0x1234 && buffer[5] == 0);

    CHECK(wcscmp(wcstok(alpha, L" ,", &alpha_state), L"red") == 0);
    CHECK(wcscmp(wcstok(words, L"|", &words_state), L"foo") == 0);
    CHECK(wcscmp(wcstok(NULL, L".", &alpha_state), L"green") == 0);
    CHECK(wcscmp(wcstok(NULL, L"|", &words_state), L"bar") == 0);
    CHECK(wcscmp(wcstok(NULL, L",", &alpha_state), L"blue") == 0);
    CHECK(wcstok(NULL, L",", &alpha_state) == NULL &&
          wcstok(NULL, L"|", &words_state) == NULL);
    return check_integers();
}
