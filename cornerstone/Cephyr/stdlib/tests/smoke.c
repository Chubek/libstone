#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_ints(const void *lhs, const void *rhs)
{
    int a = *(const int *)lhs;
    int b = *(const int *)rhs;
    return (a > b) - (a < b);
}

int main(void)
{
    char moved[16] = "abcdef";
    char text[64];
    char *heap;
    char *end;
    int values[] = {4, 1, 3, 2};
    int key = 3;
    long number;
    ptrdiff_t written;
    bool okay = true;

    _Static_assert(sizeof(size_t) == 8, "LP64 size_t");
    _Static_assert(sizeof(intptr_t) == 8, "LP64 intptr_t");
    _Static_assert(CHAR_MIN == ((char)-1 < 0 ? SCHAR_MIN : 0), "plain char signedness");
    _Static_assert(alignof(max_align_t) >= 16, "maximum alignment");

    memmove(moved + 2, moved, 6);
    assert(strcmp(moved, "ababcdef") == 0);
    assert(memchr(moved, 'd', sizeof(moved)) == moved + 5);
    assert(strstr(moved, "cde") == moved + 4);
    assert(strcoll("aardvark", "abacus") < 0);
    assert(strxfrm(NULL, "cat", 0) == 3);
    assert(strxfrm(text, "cat", sizeof(text)) == 3 && strcmp(text, "cat") == 0);
    assert(isxdigit('F') && !isxdigit('g') && toupper('q') == 'Q');

    errno = 0;
    number = strtol(" -0x2a!", &end, 0);
    assert(number == -42 && *end == '!' && errno == 0);
    number = strtol("999999999999999999999999", &end, 10);
    assert(number == LONG_MAX && errno == ERANGE && *end == '\0');

    qsort(values, 4, sizeof(values[0]), compare_ints);
    assert(values[0] == 1 && values[1] == 2 && values[2] == 3 &&
           values[3] == 4);
    assert(bsearch(&key, values, 4, sizeof(values[0]), compare_ints) ==
           &values[2]);

    assert(snprintf(text, sizeof(text), "%#08x %-5ld %.3s", 42U, -7L,
                    "abcdef") == 18);
    assert(strcmp(text, "0x00002a -7    abc") == 0);
    assert(snprintf(text, 5, "abcdef") == 6);
    assert(strcmp(text, "abcd") == 0);
    assert(snprintf(text, sizeof(text), "%#.4o|%#x|%#.0x|%#.0o%zn",
                    1U, 0U, 0U, 0U, &written) == 9);
    assert(strcmp(text, "0001|0||0") == 0 && written == 9);
    assert(snprintf(NULL, 0, "%*d", INT_MAX, 1) == INT_MAX);
    errno = 0;
    assert(snprintf(text, sizeof(text), "x%*d", INT_MAX, 1) == -1 &&
           errno == EOVERFLOW);
    assert(strcmp(text, "x") == 0);
    errno = 0;
    assert(snprintf(text, sizeof(text), "wide: %ls", L"text") == -1 &&
           errno == EINVAL);

    heap = (char *)calloc(16, 1);
    assert(heap != NULL && heap[0] == '\0' && heap[15] == '\0');
    strcpy(heap, "allocator");
    heap = (char *)realloc(heap, 32);
    assert(heap != NULL && strcmp(heap, "allocator") == 0);
    free(heap);

    puts(okay ? "cephyr libc smoke: ok" : "cephyr libc smoke: bad");
    return 0;
}
