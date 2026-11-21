#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "stream.h"

/* Linux-only operations kept private to libc. */
int __cephyr_ioctl(int, unsigned long, void *);
int __cephyr_unlinkat(int, const char *, int);
int __cephyr_renameat(int, const char *, int, const char *, unsigned);
ssize_t __cephyr_getrandom(void *, size_t, unsigned);

#define LINUX_TCGETS 0x5401
#define LINUX_AT_REMOVEDIR 0x200
#define STREAM_EOF 1U
#define STREAM_ERROR 2U
#define STREAM_READ 1U
#define STREAM_WRITE 2U
#define STREAM_AUTO (-1)

struct __cephyr_FILE {
    int descriptor;
    unsigned state;
    unsigned access;
    unsigned direction;
    int append;
    int buffering;
    int pushback;
    int orientation;
    wint_t wide_pushback;
    mbstate_t conversion_state;
    int static_stream;
    unsigned char *buffer;
    size_t capacity;
    size_t first;
    size_t last;
    int owns_buffer;
    FILE *next;
    unsigned char default_buffer[BUFSIZ];
};

static FILE stdin_object = {
    .descriptor = 0, .access = STREAM_READ, .buffering = STREAM_AUTO,
    .pushback = EOF, .wide_pushback = WEOF, .static_stream = 1
};
static FILE stdout_object = {
    .descriptor = 1, .access = STREAM_WRITE, .buffering = STREAM_AUTO,
    .pushback = EOF, .wide_pushback = WEOF, .static_stream = 1, .next = &stdin_object
};
static FILE stderr_object = {
    .descriptor = 2, .access = STREAM_WRITE, .buffering = _IONBF,
    .pushback = EOF, .wide_pushback = WEOF, .static_stream = 1, .next = &stdout_object
};

FILE *stdin = &stdin_object;
FILE *stdout = &stdout_object;
FILE *stderr = &stderr_object;
static FILE *streams = &stderr_object;

static void select_buffer(FILE *stream)
{
    if (stream->buffering == STREAM_AUTO) {
        /* TCGETS uses the same request on these Linux targets; allow room
           for the kernel termios without exposing its layout. */
        unsigned long termios[8];
        int saved_errno = errno;
        stream->buffering =
            __cephyr_ioctl(stream->descriptor, LINUX_TCGETS, termios) == 0
                ? _IOLBF
                : _IOFBF;
        errno = saved_errno;
        stream->buffer = stream->default_buffer;
        stream->capacity = sizeof(stream->default_buffer);
    }
}

static int stream_error(FILE *stream, int error)
{
    if (stream != NULL)
        stream->state |= STREAM_ERROR;
    errno = error;
    return EOF;
}

int fwide(FILE *stream, int mode)
{
    if (stream->orientation == 0 && mode != 0)
        stream->orientation = (mode > 0) - (mode < 0);
    return stream->orientation;
}

int __cephyr_stdio_orient(FILE *stream, int orientation)
{
    return stream != NULL && fwide(stream, orientation) == orientation
           ? 0 : stream_error(stream, EINVAL);
}

static size_t write_bytes(FILE *stream, const unsigned char *data, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t result = write(stream->descriptor, data + done, size - done);
        if (result > 0)
            done += (size_t)result;
        else if (result == 0 || errno != EINTR) {
            stream_error(stream, result == 0 ? EIO : errno);
            break;
        }
    }
    return done;
}

static int flush_output(FILE *stream)
{
    size_t done;
    if (stream->direction != STREAM_WRITE || stream->last == 0)
        return 0;
    done = write_bytes(stream, stream->buffer, stream->last);
    stream->last -= done;
    if (stream->last == 0)
        return 0;
    if (done != 0)
        memmove(stream->buffer, stream->buffer + done, stream->last);
    return EOF;
}

int fflush(FILE *stream)
{
    int result = 0;
    if (stream != NULL)
        return flush_output(stream);
    for (stream = streams; stream != NULL; stream = stream->next) {
        if (flush_output(stream) == EOF)
            result = EOF;
    }
    return result;
}

void __cephyr_stdio_flush_all(void)
{
    (void)fflush(NULL);
}

int setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
    int owns_buffer = 0;
    if (stream->descriptor < 0 || stream->direction != 0 ||
        (mode != _IONBF && mode != _IOFBF && mode != _IOLBF) ||
        (mode != _IONBF && size == 0 && buffer != NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (mode == _IONBF) {
        buffer = NULL;
        size = 0;
    } else if (buffer == NULL) {
        if (size == 0)
            size = sizeof(stream->default_buffer);
        if (size <= sizeof(stream->default_buffer)) {
            buffer = (char *)stream->default_buffer;
        } else {
            buffer = malloc(size);
            if (buffer == NULL)
                return -1;
            owns_buffer = 1;
        }
    }
    if (stream->owns_buffer)
        free(stream->buffer);
    stream->owns_buffer = owns_buffer;
    stream->buffering = mode;
    stream->buffer = (unsigned char *)buffer;
    stream->capacity = size;
    return 0;
}

void setbuf(FILE *stream, char *buffer)
{
    (void)setvbuf(stream, buffer, buffer == NULL ? _IONBF : _IOFBF, BUFSIZ);
}

void clearerr(FILE *stream)
{
    stream->state = 0;
}

int feof(FILE *stream)
{
    return (stream->state & STREAM_EOF) != 0;
}

int ferror(FILE *stream)
{
    return (stream->state & STREAM_ERROR) != 0;
}

int ungetc(int ch, FILE *stream)
{
    if (ch == EOF || __cephyr_stdio_orient(stream, CEPHYR_STREAM_BYTE) == EOF ||
        stream->pushback != EOF || stream->descriptor < 0 ||
        !(stream->access & STREAM_READ))
        return EOF;
    if (flush_output(stream) == EOF)
        return EOF;
    stream->direction = STREAM_READ;
    stream->pushback = (unsigned char)ch;
    stream->state &= ~STREAM_EOF;
    return stream->pushback;
}

static ssize_t read_bytes(FILE *stream, unsigned char *data, size_t size)
{
    ssize_t result;
    if (stream->buffering != _IOFBF) {
        /* Interactive input can require a prompt buffered on an output stream. */
        FILE *output;
        int saved_errno = errno;
        for (output = streams; output != NULL; output = output->next) {
            if (output->buffering == _IOLBF)
                (void)flush_output(output);
        }
        errno = saved_errno;
    }
    do {
        result = read(stream->descriptor, data, size);
    } while (result < 0 && errno == EINTR);
    if (result == 0)
        stream->state |= STREAM_EOF;
    else if (result < 0)
        stream->state |= STREAM_ERROR;
    return result;
}

static size_t stream_read(FILE *stream, unsigned char *data, size_t total)
{
    size_t done = 0;

    if (stream->descriptor < 0 || !(stream->access & STREAM_READ)) {
        stream_error(stream, EBADF);
        return 0;
    }
    if (feof(stream) || flush_output(stream) == EOF)
        return 0;
    select_buffer(stream);
    stream->direction = STREAM_READ;
    if (stream->pushback != EOF) {
        data[done++] = (unsigned char)stream->pushback;
        stream->pushback = EOF;
    }
    while (done < total) {
        size_t amount = stream->last - stream->first;
        if (amount != 0) {
            if (amount > total - done)
                amount = total - done;
            memcpy(data + done, stream->buffer + stream->first, amount);
            stream->first += amount;
            done += amount;
        } else if (stream->capacity == 0 || total - done >= stream->capacity) {
            ssize_t result = read_bytes(stream, data + done, total - done);
            if (result <= 0)
                break;
            done += (size_t)result;
        } else {
            ssize_t result = read_bytes(stream, stream->buffer, stream->capacity);
            if (result <= 0)
                break;
            stream->first = 0;
            stream->last = (size_t)result;
        }
    }
    return done;
}

static size_t stream_write(FILE *stream, const unsigned char *data, size_t total)
{
    size_t done = 0;

    if (stream->descriptor < 0 || !(stream->access & STREAM_WRITE)) {
        stream_error(stream, EBADF);
        return 0;
    }
    select_buffer(stream);
    if (stream->direction != STREAM_WRITE) {
        stream->first = stream->last = 0;
        stream->pushback = EOF;
        stream->wide_pushback = WEOF;
        stream->direction = STREAM_WRITE;
    }
    if (stream->capacity == 0)
        return write_bytes(stream, data, total);
    while (done < total) {
        size_t amount = stream->capacity - stream->last;
        int line = 0;
        if (amount > total - done)
            amount = total - done;
        if (stream->buffering == _IOLBF && amount != 0) {
            const unsigned char *newline = memchr(data + done, '\n', amount);
            if (newline != NULL) {
                amount = (size_t)(newline - (data + done)) + 1;
                line = 1;
            }
        }
        if (amount != 0) {
            memcpy(stream->buffer + stream->last, data + done, amount);
            stream->last += amount;
            done += amount;
        }
        if (line || stream->last == stream->capacity) {
            if (flush_output(stream) == EOF) {
                done -= stream->last < done ? stream->last : done;
                break;
            }
        }
    }
    return done;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *stream)
{
    if (size == 0 || count == 0)
        return 0;
    if (__cephyr_stdio_orient(stream, CEPHYR_STREAM_BYTE) == EOF)
        return 0;
    if (count > SIZE_MAX / size) {
        stream_error(stream, EINVAL);
        return 0;
    }
    return stream_read(stream, ptr, size * count) / size;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream)
{
    if (size == 0 || count == 0)
        return 0;
    if (__cephyr_stdio_orient(stream, CEPHYR_STREAM_BYTE) == EOF)
        return 0;
    if (count > SIZE_MAX / size) {
        stream_error(stream, EINVAL);
        return 0;
    }
    return stream_write(stream, ptr, size * count) / size;
}

void perror(const char *prefix)
{
    const char *message = strerror(errno);

    /* perror is not a byte I/O function (C11 7.21.1). Neither orient stderr
       nor reject a wide-oriented stderr. The locale message is ASCII. */
    if (prefix != NULL && *prefix != '\0') {
        (void)stream_write(stderr, (const unsigned char *)prefix, strlen(prefix));
        (void)stream_write(stderr, (const unsigned char *)": ", 2);
    }
    (void)stream_write(stderr, (const unsigned char *)message, strlen(message));
    (void)stream_write(stderr, (const unsigned char *)"\n", 1);
}

wint_t fgetwc(FILE *stream)
{
    unsigned char byte;
    wchar_t wc;
    if (__cephyr_stdio_orient(stream, CEPHYR_STREAM_WIDE) == EOF)
        return WEOF;
    if (stream->wide_pushback != WEOF) {
        wint_t pushed = stream->wide_pushback;
        stream->wide_pushback = WEOF;
        return pushed;
    }
    if (stream_read(stream, &byte, 1) != 1)
        return WEOF;
    /* The C locale encodes each wide character in exactly one byte. */
    if (mbrtowc(&wc, (const char *)&byte, 1, &stream->conversion_state) > 1) {
        stream_error(stream, EILSEQ);
        return WEOF;
    }
    return (wint_t)wc;
}

wchar_t *fgetws(wchar_t *restrict str, int count, FILE *restrict stream)
{
    int used = 0;
    if (count <= 0 || __cephyr_stdio_orient(stream, CEPHYR_STREAM_WIDE) == EOF)
        return NULL;
    if (count == 1) {
        *str = 0;
        return str;
    }
    while (used < count - 1) {
        wint_t wc = fgetwc(stream);
        if (wc == WEOF) {
            if (!feof(stream) || used == 0)
                return NULL;
            break;
        }
        str[used++] = (wchar_t)wc;
        if (wc == L'\n')
            break;
    }
    str[used] = 0;
    return str;
}

wint_t fputwc(wchar_t wc, FILE *stream)
{
    char bytes[MB_LEN_MAX];
    size_t length;
    if (__cephyr_stdio_orient(stream, CEPHYR_STREAM_WIDE) == EOF)
        return WEOF;
    length = wcrtomb(bytes, wc, &stream->conversion_state);
    if (length == (size_t)-1) {
        stream->state |= STREAM_ERROR;
        return WEOF;
    }
    return stream_write(stream, (const unsigned char *)bytes, length) == length
           ? (wint_t)wc : WEOF;
}

int fputws(const wchar_t *restrict str, FILE *restrict stream)
{
    if (__cephyr_stdio_orient(stream, CEPHYR_STREAM_WIDE) == EOF)
        return EOF;
    while (*str != 0) {
        if (fputwc(*str++, stream) == WEOF)
            return EOF;
    }
    return 0;
}

wint_t getwc(FILE *stream)
{
    return fgetwc(stream);
}

wint_t getwchar(void)
{
    return fgetwc(stdin);
}

wint_t putwc(wchar_t wc, FILE *stream)
{
    return fputwc(wc, stream);
}

wint_t putwchar(wchar_t wc)
{
    return fputwc(wc, stdout);
}

wint_t ungetwc(wint_t wc, FILE *stream)
{
    if (wc == WEOF || __cephyr_stdio_orient(stream, CEPHYR_STREAM_WIDE) == EOF ||
        stream->wide_pushback != WEOF || stream->descriptor < 0 ||
        !(stream->access & STREAM_READ) || flush_output(stream) == EOF)
        return WEOF;
    stream->direction = STREAM_READ;
    stream->wide_pushback = wc;
    stream->state &= ~STREAM_EOF;
    return wc;
}

static size_t unread(const FILE *stream)
{
    /* The logical position for wide pushback is unspecified; use the
       position of the next byte in the file, ignoring wide pushback. */
    return stream->last - stream->first + (stream->pushback != EOF);
}

int fseek(FILE *stream, long offset, int whence)
{
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    if (flush_output(stream) == EOF)
        return -1;
    if (whence == SEEK_CUR && stream->direction == STREAM_READ) {
        size_t pending = unread(stream);
        if (pending > (size_t)LONG_MAX || offset < LONG_MIN + (long)pending) {
            errno = EOVERFLOW;
            return -1;
        }
        offset -= (long)pending;
    }
    if (lseek(stream->descriptor, offset, whence) == (off_t)-1)
        return -1;
    stream->first = stream->last = 0;
    stream->direction = 0;
    stream->pushback = EOF;
    stream->wide_pushback = WEOF;
    stream->conversion_state = 0;
    stream->state &= ~STREAM_EOF;
    return 0;
}

long ftell(FILE *stream)
{
    long result = lseek(stream->descriptor, 0,
        stream->append && stream->direction == STREAM_WRITE && stream->last != 0
            ? SEEK_END
            : SEEK_CUR);
    if (result == -1)
        return -1;
    if (stream->direction == STREAM_READ) {
        size_t pending = unread(stream);
        if (pending > (size_t)result) {
            errno = EINVAL;
            return -1;
        }
        return result - (long)pending;
    }
    if (stream->direction == STREAM_WRITE) {
        if (stream->last > (size_t)(LONG_MAX - result)) {
            errno = EOVERFLOW;
            return -1;
        }
        return result + (long)stream->last;
    }
    return result;
}

int fgetpos(FILE *stream, fpos_t *position)
{
    long result = ftell(stream);
    if (result == -1)
        return -1;
    position->__offset = result;
    position->__state = stream->conversion_state;
    return 0;
}

int fsetpos(FILE *stream, const fpos_t *position)
{
    if (fseek(stream, position->__offset, SEEK_SET) != 0)
        return -1;
    stream->conversion_state = position->__state;
    return 0;
}

void rewind(FILE *stream)
{
    (void)fseek(stream, 0, SEEK_SET);
    clearerr(stream);
}

static int open_flags(const char *mode)
{
    int flags;
    int update = 0;
    int binary = 0;
    if (*mode == 'r')
        flags = O_RDONLY;
    else if (*mode == 'w')
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (*mode == 'a')
        flags = O_WRONLY | O_CREAT | O_APPEND;
    else
        return -1;

    while (*++mode != '\0') {
        if (*mode == '+' && !update)
            update = 1;
        else if (*mode == 'b' && !binary)
            binary = 1;
        else if (*mode == 'x' && (flags & O_TRUNC) && mode[1] == '\0')
            flags |= O_EXCL;
        else
            return -1;
    }
    if (update)
        flags = (flags & ~O_ACCMODE) | O_RDWR;
    return flags;
}

static int open_path(const char *path, const char *mode, int *flags)
{
    int descriptor;
    *flags = open_flags(mode);
    if (*flags < 0) {
        errno = EINVAL;
        return -1;
    }
    do {
        descriptor = open(path, *flags, 0666);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor >= 0 && (*flags & (O_APPEND | O_ACCMODE)) ==
                           (O_APPEND | O_WRONLY)) {
        int saved_errno = errno;
        (void)lseek(descriptor, 0, SEEK_END);
        errno = saved_errno;
    }
    return descriptor;
}

static void attach(FILE *stream, int descriptor, int flags)
{
    stream->descriptor = descriptor;
    stream->state = stream->direction = 0;
    stream->access = (flags & O_ACCMODE) == O_RDONLY ? STREAM_READ
                   : (flags & O_ACCMODE) == O_WRONLY ? STREAM_WRITE
                                                    : STREAM_READ | STREAM_WRITE;
    stream->append = flags & O_APPEND;
    stream->buffering = STREAM_AUTO;
    stream->pushback = EOF;
    stream->orientation = 0;
    stream->wide_pushback = WEOF;
    stream->conversion_state = 0;
}

static FILE *new_stream(int descriptor, int flags)
{
    FILE *stream = malloc(sizeof(*stream));
    if (stream == NULL) {
        int saved_errno = errno;
        (void)close(descriptor);
        errno = saved_errno;
        return NULL;
    }
    *stream = (FILE){0};
    attach(stream, descriptor, flags);
    stream->next = streams;
    streams = stream;
    return stream;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags;
    int descriptor = open_path(path, mode, &flags);
    return descriptor < 0 ? NULL : new_stream(descriptor, flags);
}

static int detach(FILE *stream)
{
    int result = 0;
    int saved_errno = errno;
    if (stream->descriptor >= 0) {
        result = flush_output(stream);
        saved_errno = errno;
        /* Linux closes the descriptor even if close reports EINTR. */
        if (close(stream->descriptor) < 0 && result == 0) {
            result = EOF;
            saved_errno = errno;
        }
        stream->descriptor = -1;
    }
    if (stream->owns_buffer)
        free(stream->buffer);
    stream->buffer = NULL;
    stream->owns_buffer = 0;
    stream->capacity = stream->first = stream->last = 0;
    stream->direction = 0;
    errno = saved_errno;
    return result;
}

static void destroy(FILE *stream)
{
    FILE **link = &streams;
    if (stream->static_stream)
        return;
    while (*link != stream)
        link = &(*link)->next;
    *link = stream->next;
    free(stream);
}

int fclose(FILE *stream)
{
    int result = detach(stream);
    destroy(stream);
    return result;
}

FILE *freopen(const char *path, const char *mode, FILE *stream)
{
    int flags;
    int descriptor;
    (void)detach(stream);
    /* Cephyr does not support mode-only changes. */
    if (path == NULL) {
        errno = EINVAL;
        destroy(stream);
        return NULL;
    }
    descriptor = open_path(path, mode, &flags);
    if (descriptor < 0) {
        destroy(stream);
        return NULL;
    }
    attach(stream, descriptor, flags);
    return stream;
}

int remove(const char *path)
{
    if (__cephyr_unlinkat(AT_FDCWD, path, 0) == 0)
        return 0;
    if (errno != EISDIR)
        return -1;
    return __cephyr_unlinkat(AT_FDCWD, path, LINUX_AT_REMOVEDIR);
}

int rename(const char *old_path, const char *new_path)
{
    return __cephyr_renameat(AT_FDCWD, old_path, AT_FDCWD, new_path, 0);
}

static int temp_name(char *name)
{
    static uint64_t random_prefix;
    static uint64_t serial;
    static const char hex[] = "0123456789abcdef";
    size_t i;

    if (serial == 0) {
        unsigned char *bytes = (unsigned char *)&random_prefix;
        size_t filled = 0;
        while (filled < sizeof(random_prefix)) {
            ssize_t n = __cephyr_getrandom(bytes + filled,
                                           sizeof(random_prefix) - filled, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n == 0) {
                errno = EIO;
                return -1;
            }
            if (n < 0)
                return -1;
            filled += (size_t)n;
        }
    }
    if (serial == UINT64_MAX) {
        errno = EEXIST;
        return -1;
    }
    ++serial;
    memcpy(name, "/tmp/cephyr-", 12);
    for (i = 0; i < 16; ++i) {
        name[12 + i] = hex[(random_prefix >> ((15 - i) * 4)) & 15];
        name[28 + i] = hex[(serial >> ((15 - i) * 4)) & 15];
    }
    name[44] = '\0';
    return 0;
}

char *tmpnam(char *buffer)
{
    static char internal[L_tmpnam];
    char *name = buffer == NULL ? internal : buffer;
    int attempts;
    for (attempts = 0; attempts < 128; ++attempts) {
        int descriptor;
        int saved_errno = errno;
        if (temp_name(name) < 0)
            return NULL;
        do {
            descriptor = open(name, O_PATH | O_NOFOLLOW, 0);
        } while (descriptor < 0 && errno == EINTR);
        if (descriptor < 0) {
            if (errno != ENOENT)
                return NULL;
            errno = saved_errno;
            return name;
        }
        (void)close(descriptor);
    }
    errno = EEXIST;
    return NULL;
}

FILE *tmpfile(void)
{
    int descriptor;
    int attempts;
    do {
        descriptor = open("/tmp", O_TMPFILE | O_RDWR | O_EXCL, 0600);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor >= 0)
        return new_stream(descriptor, O_RDWR);

    for (attempts = 0; attempts < 128; ++attempts) {
        char name[L_tmpnam];
        if (temp_name(name) < 0)
            return NULL;
        do {
            descriptor = open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        } while (descriptor < 0 && errno == EINTR);
        if (descriptor >= 0) {
            if (remove(name) != 0) {
                int saved_errno = errno;
                (void)close(descriptor);
                errno = saved_errno;
                return NULL;
            }
            return new_stream(descriptor, O_RDWR);
        }
        if (errno != EEXIST)
            return NULL;
    }
    errno = EEXIST;
    return NULL;
}
