#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t count)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    while (count != 0) {
        *d++ = *s++;
        --count;
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t count)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || count == 0)
        return dest;
    if ((uintptr_t)d < (uintptr_t)s) {
        while (count != 0) {
            *d++ = *s++;
            --count;
        }
    } else {
        d += count;
        s += count;
        while (count != 0) {
            *--d = *--s;
            --count;
        }
    }
    return dest;
}

void *memset(void *dest, int value, size_t count)
{
    unsigned char *d = (unsigned char *)dest;

    while (count != 0) {
        *d++ = (unsigned char)value;
        --count;
    }
    return dest;
}

int memcmp(const void *lhs, const void *rhs, size_t count)
{
    const unsigned char *a = (const unsigned char *)lhs;
    const unsigned char *b = (const unsigned char *)rhs;

    while (count != 0) {
        if (*a != *b)
            return *a < *b ? -1 : 1;
        ++a;
        ++b;
        --count;
    }
    return 0;
}

void *memchr(const void *ptr, int value, size_t count)
{
    const unsigned char *p = (const unsigned char *)ptr;
    unsigned char byte = (unsigned char)value;

    while (count != 0) {
        if (*p == byte)
            return (void *)p;
        ++p;
        --count;
    }
    return NULL;
}

size_t strlen(const char *str)
{
    const char *end = str;
    while (*end != '\0')
        ++end;
    return (size_t)(end - str);
}

size_t strnlen(const char *str, size_t max_count)
{
    size_t length = 0;
    while (length < max_count && str[length] != '\0')
        ++length;
    return length;
}

char *strcpy(char *restrict dest, const char *restrict src)
{
    char *result = dest;
    do {
        *dest++ = *src;
    } while (*src++ != '\0');
    return result;
}

char *strncpy(char *restrict dest, const char *restrict src, size_t count)
{
    char *result = dest;

    while (count != 0 && *src != '\0') {
        *dest++ = *src++;
        --count;
    }
    while (count != 0) {
        *dest++ = '\0';
        --count;
    }
    return result;
}

char *strcat(char *restrict dest, const char *restrict src)
{
    char *result = dest;
    while (*dest != '\0')
        ++dest;
    do {
        *dest++ = *src;
    } while (*src++ != '\0');
    return result;
}

char *strncat(char *restrict dest, const char *restrict src, size_t count)
{
    char *result = dest;
    while (*dest != '\0')
        ++dest;
    while (count != 0 && *src != '\0') {
        *dest++ = *src++;
        --count;
    }
    *dest = '\0';
    return result;
}

int strcmp(const char *lhs, const char *rhs)
{
    const unsigned char *a = (const unsigned char *)lhs;
    const unsigned char *b = (const unsigned char *)rhs;

    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    if (*a == *b)
        return 0;
    return *a < *b ? -1 : 1;
}

int strncmp(const char *lhs, const char *rhs, size_t count)
{
    const unsigned char *a = (const unsigned char *)lhs;
    const unsigned char *b = (const unsigned char *)rhs;

    while (count != 0) {
        if (*a != *b)
            return *a < *b ? -1 : 1;
        if (*a == '\0')
            return 0;
        ++a;
        ++b;
        --count;
    }
    return 0;
}

int strcoll(const char *lhs, const char *rhs)
{
    return strcmp(lhs, rhs);
}

size_t strxfrm(char *restrict dest, const char *restrict src, size_t count)
{
    size_t length = strlen(src);
    if (length < count)
        memcpy(dest, src, length + 1);
    return length;
}

char *strchr(const char *str, int ch)
{
    char target = (char)ch;
    for (;;) {
        if (*str == target)
            return (char *)str;
        if (*str == '\0')
            return NULL;
        ++str;
    }
}

char *strrchr(const char *str, int ch)
{
    const char *last = NULL;
    char target = (char)ch;

    do {
        if (*str == target)
            last = str;
    } while (*str++ != '\0');
    return (char *)last;
}

static int char_in_set(unsigned char ch, const char *set)
{
    while (*set != '\0') {
        if (ch == (unsigned char)*set++)
            return 1;
    }
    return 0;
}

size_t strspn(const char *str, const char *accept)
{
    size_t count = 0;
    while (str[count] != '\0' &&
           char_in_set((unsigned char)str[count], accept))
        ++count;
    return count;
}

size_t strcspn(const char *str, const char *reject)
{
    size_t count = 0;
    while (str[count] != '\0' &&
           !char_in_set((unsigned char)str[count], reject))
        ++count;
    return count;
}

char *strpbrk(const char *str, const char *breakset)
{
    while (*str != '\0') {
        if (char_in_set((unsigned char)*str, breakset))
            return (char *)str;
        ++str;
    }
    return NULL;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t needle_length;

    if (*needle == '\0')
        return (char *)haystack;
    needle_length = strlen(needle);
    while (*haystack != '\0') {
        if (*haystack == *needle &&
            strncmp(haystack, needle, needle_length) == 0)
            return (char *)haystack;
        ++haystack;
    }
    return NULL;
}

char *strtok(char *restrict str, const char *restrict delimiters)
{
    static char *next;
    char *token;

    if (str != NULL)
        next = str;
    if (next == NULL)
        return NULL;

    next += strspn(next, delimiters);
    if (*next == '\0') {
        next = NULL;
        return NULL;
    }

    token = next;
    next += strcspn(next, delimiters);
    if (*next != '\0')
        *next++ = '\0';
    else
        next = NULL;
    return token;
}

char *strerror(int errnum)
{
    switch (errnum) {
    case 0: return "No error";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case ESRCH: return "No such process";
    case EINTR: return "Interrupted system call";
    case EIO: return "Input/output error";
    case ENXIO: return "No such device or address";
    case E2BIG: return "Argument list too long";
    case ENOEXEC: return "Exec format error";
    case EBADF: return "Bad file descriptor";
    case ECHILD: return "No child processes";
    case EAGAIN: return "Resource temporarily unavailable";
    case ENOMEM: return "Cannot allocate memory";
    case EACCES: return "Permission denied";
    case EFAULT: return "Bad address";
    case EBUSY: return "Device or resource busy";
    case EEXIST: return "File exists";
    case EXDEV: return "Invalid cross-device link";
    case ENODEV: return "No such device";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case ENFILE: return "Too many open files in system";
    case EMFILE: return "Too many open files";
    case ENOTTY: return "Inappropriate ioctl for device";
    case EFBIG: return "File too large";
    case ENOSPC: return "No space left on device";
    case ESPIPE: return "Illegal seek";
    case EROFS: return "Read-only file system";
    case EMLINK: return "Too many links";
    case EPIPE: return "Broken pipe";
    case EDOM: return "Numerical argument out of domain";
    case ERANGE: return "Numerical result out of range";
    case ENOSYS: return "Function not implemented";
    case EOVERFLOW: return "Value too large for data type";
    case EILSEQ: return "Invalid or incomplete multibyte character";
    default: return "Unknown error";
    }
}

wchar_t *wcscpy(wchar_t *restrict dest, const wchar_t *restrict src)
{
    wchar_t *result = dest;
    do {
        *dest++ = *src;
    } while (*src++ != 0);
    return result;
}

wchar_t *wcsncpy(wchar_t *restrict dest, const wchar_t *restrict src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i] != 0) {
        dest[i] = src[i];
        ++i;
    }
    while (i < n)
        dest[i++] = 0;
    return dest;
}

wchar_t *wmemcpy(wchar_t *restrict dest, const wchar_t *restrict src, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        dest[i] = src[i];
    return dest;
}

wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n)
{
    if (dest == src)
        return dest;
    if ((uintptr_t)dest < (uintptr_t)src) {
        for (size_t i = 0; i < n; ++i)
            dest[i] = src[i];
    } else {
        while (n != 0) {
            --n;
            dest[n] = src[n];
        }
    }
    return dest;
}

wchar_t *wcscat(wchar_t *restrict dest, const wchar_t *restrict src)
{
    wcscpy(dest + wcslen(dest), src);
    return dest;
}

wchar_t *wcsncat(wchar_t *restrict dest, const wchar_t *restrict src, size_t n)
{
    wchar_t *end = dest + wcslen(dest);
    while (n != 0 && *src != 0) {
        *end++ = *src++;
        --n;
    }
    *end = 0;
    return dest;
}

static int wide_compare(wchar_t a, wchar_t b)
{
    return (a > b) - (a < b);
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a != 0 && *a == *b) {
        ++a;
        ++b;
    }
    return wide_compare(*a, *b);
}

int wcscoll(const wchar_t *a, const wchar_t *b)
{
    return wcscmp(a, b);
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i])
            return wide_compare(a[i], b[i]);
        if (a[i] == 0)
            break;
    }
    return 0;
}

size_t wcsxfrm(wchar_t *restrict dest, const wchar_t *restrict src, size_t n)
{
    /* Numeric ordering of wide characters is the C locale's collation. */
    size_t len = wcslen(src);
    if (len < n)
        wmemcpy(dest, src, len + 1);
    return len;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i])
            return wide_compare(a[i], b[i]);
    }
    return 0;
}

wchar_t *wcschr(const wchar_t *str, wchar_t wc)
{
    for (;;) {
        if (*str == wc)
            return (wchar_t *)str;
        if (*str == 0)
            return NULL;
        ++str;
    }
}

size_t wcscspn(const wchar_t *str, const wchar_t *reject)
{
    size_t n = 0;
    while (str[n] != 0 && wcschr(reject, str[n]) == NULL)
        ++n;
    return n;
}

wchar_t *wcspbrk(const wchar_t *str, const wchar_t *accept)
{
    size_t n = wcscspn(str, accept);
    return str[n] == 0 ? NULL : (wchar_t *)str + n;
}

wchar_t *wcsrchr(const wchar_t *str, wchar_t wc)
{
    const wchar_t *last = NULL;
    do {
        if (*str == wc)
            last = str;
    } while (*str++ != 0);
    return (wchar_t *)last;
}

size_t wcsspn(const wchar_t *str, const wchar_t *accept)
{
    size_t n = 0;
    while (str[n] != 0 && wcschr(accept, str[n]) != NULL)
        ++n;
    return n;
}

wchar_t *wcsstr(const wchar_t *str, const wchar_t *needle)
{
    size_t len = wcslen(needle);
    if (len == 0)
        return (wchar_t *)str;
    for (; *str != 0; ++str) {
        if (*str == *needle && wcsncmp(str, needle, len) == 0)
            return (wchar_t *)str;
    }
    return NULL;
}

wchar_t *wcstok(wchar_t *restrict str, const wchar_t *restrict delimiters,
                wchar_t **restrict state)
{
    if (str == NULL)
        str = *state;
    if (str != NULL) {
        str += wcsspn(str, delimiters);
        if (*str != 0) {
            wchar_t *end = str + wcscspn(str, delimiters);
            if (*end != 0) {
                *end = 0;
                *state = end + 1;
            } else {
                *state = NULL;
            }
            return str;
        }
    }
    *state = NULL;
    return NULL;
}

wchar_t *wmemchr(const wchar_t *str, wchar_t wc, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (str[i] == wc)
            return (wchar_t *)str + i;
    }
    return NULL;
}

size_t wcslen(const wchar_t *str)
{
    size_t n = 0;
    while (str[n] != 0)
        ++n;
    return n;
}

wchar_t *wmemset(wchar_t *dest, wchar_t wc, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        dest[i] = wc;
    return dest;
}
