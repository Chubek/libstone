#ifndef _CEPHYR_UNISTD_H
#define _CEPHYR_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#ifdef __cplusplus
extern "C" {
#endif

extern char **environ;

ssize_t read(int descriptor, void *buffer, size_t count);
ssize_t write(int descriptor, const void *buffer, size_t count);
int close(int descriptor);
off_t lseek(int descriptor, off_t offset, int whence);
void _exit(int status);

#ifdef __cplusplus
}
#endif

#endif
