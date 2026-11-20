#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(__LP64__) || defined(__riscv)
#error wrong data model or target
#elif defined(__aarch64__)
#if !defined(__AARCH64EL__) || !defined(__ARM_PCS_AAPCS64) || !defined(__CHAR_UNSIGNED__)
#error wrong AArch64 built-in target macros
#endif
#define HIGH_CHAR 255
_Static_assert(CHAR_MIN == 0 && CHAR_MAX == 255, "AArch64 plain char limits");
_Static_assert(WCHAR_MIN == 0 && (wchar_t)-1 > 0, "AArch64 wchar_t");
#elif defined(__x86_64__) && !defined(__CHAR_UNSIGNED__)
#define HIGH_CHAR (-1)
_Static_assert(CHAR_MIN == -128 && CHAR_MAX == 127, "AMD64 plain char limits");
#else
#error wrong built-in target macros
#endif

_Static_assert('\xff' == HIGH_CHAR, "target character literal");

int main(int argc, char **argv)
{
    char c = (char)255;
    char *memory = malloc(16);
    if (argc != 2 || !environ || strcmp(environ[0], "CEPHYR_RUNTIME_TEST=ok"))
        return 1;
    if (!memory || c != HIGH_CHAR || ((signed char)255) != -1 || ((unsigned char)255) != 255)
        return 2;
    errno = 0;
    if (close(-1) != -1 || errno != EBADF)
        return 3;
    int fd = openat(AT_FDCWD, "runtime-file", O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || write(fd, "data", 4) != 4 || lseek(fd, 0, SEEK_SET) != 0 ||
        read(fd, memory, 4) != 4 || memcmp(memory, "data", 4) || close(fd))
        return 4;
    fd = open(".", O_RDONLY | O_DIRECTORY);
    if (fd < 0 || close(fd))
        return 5;
    if (open("runtime-file", O_RDONLY | O_DIRECTORY) != -1 || errno != ENOTDIR)
        return 6;
    free(memory);
    printf("%s %d %d %d %d %d %d %d %d\n", argv[1], -7, 2, 3, 4, 5, 6, 7, 8);
    return 0;
}
