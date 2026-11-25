/* Embedded only when std.ipc is imported. Place after the runtime's types and
   prototypes, in the translation unit that defines interop_call. */
#if !defined(LLS_IPC_DISABLE) && (defined(__unix__) || defined(__APPLE__))
#define LLS_IPC_POSIX 1
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <mqueue.h>
#include <semaphore.h>
#include <sys/mman.h>
#endif
#endif

static Val lls_ipc_nil(void) {
    Val nil = LLS_ZERO;
    return nil;
}

static Val lls_ipc_invalid(Context *c, const char *op, const char *detail) {
    error(c, "IPC: %s: %s", op, detail);
    return lls_ipc_nil();
}

static Val lls_ipc_unavailable(Context *c, const char *op, const char *detail) {
    error(c, "IPC_UNAVAILABLE: %s: %s", op, detail);
    return lls_ipc_nil();
}

#ifdef LLS_IPC_POSIX

#define LLS_IPC_LIMIT (1024 * 1024)

static Val lls_ipc_error(Context *c, const char *op, int code) {
    const char *kind = "IPC";
    if (code == ENOSYS || code == ENOTSUP || code == EOPNOTSUPP)
        kind = "IPC_UNAVAILABLE";
    else if (code == EAGAIN || code == EWOULDBLOCK)
        kind = "IPC_WOULD_BLOCK";
    char buffer[160];
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    const char *detail = strerror_r(code, buffer, sizeof buffer);
#else
    const char *detail = strerror_r(code, buffer, sizeof buffer) ? "system error" : buffer;
#endif
    error(c, "%s: %s: %s (errno %d)", kind, op, detail, code);
    return lls_ipc_nil();
}

static int lls_ipc_signature(Vec *args, const char *signature) {
    if (args->n != (int)strlen(signature)) return 0;
    for (int i = 0; i < args->n; ++i) {
        Val v = args->a[i];
        if ((signature[i] == 'i' && v.tag != VINT) ||
            (signature[i] == 'b' && v.tag != VBOOL) ||
            (signature[i] == 's' && v.tag != VSTR) ||
            (signature[i] == 'v' && (v.tag != VVEC || !v.vec)))
            return 0;
    }
    return 1;
}

static Vec *lls_ipc_vec(Context *c, int n) {
    Vec *v = new_vec(c);
    if (n) v->a = (Val *)ca(c, (size_t)n * sizeof(Val));
    v->n = v->cap = n;
    return v;
}

static Val lls_ipc_bytes(Context *c, const unsigned char *bytes, int n) {
    Vec *v = lls_ipc_vec(c, n);
    for (int i = 0; i < n; ++i) v->a[i] = vint(bytes[i]);
    return vvec(v);
}

static unsigned char *lls_ipc_payload(Context *c, const char *op, const Vec *values) {
    if (values->n > LLS_IPC_LIMIT) {
        lls_ipc_invalid(c, op, "payload size exceeds 1 MiB");
        return NULL;
    }
    for (int i = 0; i < values->n; ++i) {
        if (values->a[i].tag != VINT || values->a[i].i < 0 || values->a[i].i > 255) {
            lls_ipc_invalid(c, op, "expected an octet vector (integers in 0..255)");
            return NULL;
        }
    }
    unsigned char *bytes = (unsigned char *)ca(c, (size_t)values->n);
    for (int i = 0; i < values->n; ++i) bytes[i] = (unsigned char)values->a[i].i;
    return bytes;
}

#if defined(__linux__)
static int lls_ipc_posix_name(const char *name) {
    return name[0] == '/' && name[1] && !strchr(name + 1, '/') &&
           strcmp(name, "/.") && strcmp(name, "/..");
}
#endif

static Val lls_ipc_pair(Context *c, const char *op, int sockets) {
    Vec *out = lls_ipc_vec(c, 2); /* Allocate before acquiring descriptors. */
    int fds[2];
#if defined(__linux__)
    int status = sockets ? socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds)
                         : pipe2(fds, O_CLOEXEC);
    if (status < 0) return lls_ipc_error(c, op, errno);
#else
    int status = sockets ? socketpair(AF_UNIX, SOCK_STREAM, 0, fds) : pipe(fds);
    if (status < 0) return lls_ipc_error(c, op, errno);
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(fds[0]);
        close(fds[1]);
        return lls_ipc_error(c, op, saved);
    }
#endif
    out->a[0] = vint(fds[0]);
    out->a[1] = vint(fds[1]);
    return vvec(out);
}

static Val lls_ipc_fifo(Context *c, const char *op, Vec *args) {
    const char *path = args->a[0].s;
    if (!*path) return lls_ipc_invalid(c, op, "expected a nonempty FIFO path");
    if (!strcmp(op, "fifoCreate")) {
        if (mkfifo(path, 0600) < 0) return lls_ipc_error(c, op, errno);
        return lls_ipc_nil();
    }
    struct stat st;
    if (lstat(path, &st) < 0) return lls_ipc_error(c, op, errno);
    if (!S_ISFIFO(st.st_mode)) return lls_ipc_invalid(c, op, "path is not a FIFO");
    if (!strcmp(op, "fifoUnlink")) {
        if (unlink(path) < 0) return lls_ipc_error(c, op, errno);
        return lls_ipc_nil();
    }
    int flags = truth(args->a[1]) ? O_WRONLY : O_RDONLY;
    int fd;
    do { fd = open(path, flags | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW); }
    while (fd < 0 && errno == EINTR);
    if (fd < 0) return lls_ipc_error(c, op, errno);
    if (fstat(fd, &st) < 0) {
        int saved = errno;
        close(fd);
        return lls_ipc_error(c, op, saved);
    }
    if (!S_ISFIFO(st.st_mode)) {
        close(fd);
        return lls_ipc_invalid(c, op, "path is not a FIFO");
    }
    return vint(fd);
}

/* POSIX write can terminate the host via SIGPIPE. Block it only on the calling
   thread, consuming this write's signal on EPIPE (but not an older pending one). */
static ssize_t lls_ipc_stream_write(int fd, const unsigned char *data, size_t n) {
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    int status = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (status) { errno = status; return -1; }
    if (sigpending(&pending) < 0) {
        int saved = errno;
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
        errno = saved;
        return -1;
    }
    ssize_t count;
    do { count = write(fd, data, n); } while (count < 0 && errno == EINTR);
    int saved = errno;
    if (count < 0 && saved == EPIPE && !sigismember(&pending, SIGPIPE)) {
#if defined(__linux__)
        struct timespec now = {0, 0};
        while (sigtimedwait(&blocked, NULL, &now) < 0 && errno == EINTR) {}
#else
        if (!sigpending(&pending) && sigismember(&pending, SIGPIPE)) {
            int received;
            (void)sigwait(&blocked, &received);
        }
#endif
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    errno = saved;
    return count;
}

static Val lls_ipc_wait(Context *c, const char *op, int fd, int events, int64_t duration) {
    if (duration < 0 || duration > INT_MAX)
        return lls_ipc_invalid(c, op, "timeout in milliseconds must be in 0..INT_MAX");
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) return lls_ipc_error(c, op, errno);
    struct pollfd p = {fd, (short)events, 0};
    int remaining = (int)duration;
    for (;;) {
        int status = poll(&p, 1, remaining);
        if (status >= 0) {
            if (status && (p.revents & POLLNVAL)) return lls_ipc_error(c, op, EBADF);
            return vbool(status != 0);
        }
        if (errno != EINTR) return lls_ipc_error(c, op, errno);
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return lls_ipc_error(c, op, errno);
        int64_t elapsed_ns = ((int64_t)now.tv_sec - (int64_t)start.tv_sec) * INT64_C(1000000000)
                             + ((int64_t)now.tv_nsec - (int64_t)start.tv_nsec);
        if (elapsed_ns >= duration * INT64_C(1000000)) return vbool(0);
        remaining = (int)(duration - elapsed_ns / INT64_C(1000000));
    }
}

#if defined(__linux__)
static Val lls_ipc_shm_open(Context *c, const char *op, Vec *args) {
    const char *name = args->a[0].s;
    if (!lls_ipc_posix_name(name))
        return lls_ipc_invalid(c, op, "expected a POSIX name of the form /name");
    if (!strcmp(op, "shmUnlink")) {
        if (shm_unlink(name) < 0) return lls_ipc_error(c, op, errno);
        return lls_ipc_nil();
    }
    int create = !strcmp(op, "shmCreate");
    int64_t size = create ? args->a[1].i : 0;
    if (create && (size < 0 || size > INT64_C(1073741824)))
        return lls_ipc_invalid(c, op, "shared memory size must be in 0..1073741824");
    int flags = create ? O_RDWR | O_CREAT | O_EXCL : truth(args->a[1]) ? O_RDWR : O_RDONLY;
    int fd = shm_open(name, flags, 0600); /* shm_open sets FD_CLOEXEC. */
    if (fd < 0) {
        if (create && errno == ENOENT)
            return lls_ipc_unavailable(c, op, "cannot create the POSIX shared memory object; check the host namespace (Linux: /dev/shm)");
        return lls_ipc_error(c, op, errno);
    }
    if (create) {
        int status;
        do { status = ftruncate(fd, (off_t)size); } while (status < 0 && errno == EINTR);
        if (status < 0) {
            int saved = errno;
            close(fd);
            shm_unlink(name);
            return lls_ipc_error(c, op, saved);
        }
    }
    return vint(fd);
}
#endif

static Val lls_ipc_io(Context *c, const char *op, int fd, Vec *args) {
    int shared = !strncmp(op, "shm", 3);
    int writing = !strcmp(op, "write") || !strcmp(op, "shmWrite");
    int index = shared ? 2 : 1;
    off_t offset = 0;
    struct stat st = {0};
    if (shared) {
        if (fstat(fd, &st) < 0) return lls_ipc_error(c, op, errno);
        if (!S_ISREG(st.st_mode)) return lls_ipc_invalid(c, op, "expected a shared memory descriptor");
        if (!strcmp(op, "shmSize")) return vint((int64_t)st.st_size);
        int64_t value = args->a[1].i;
        offset = (off_t)value;
        if (value < 0 || (int64_t)offset != value)
            return lls_ipc_invalid(c, op, "offset is outside the host file-offset range");
    }
    ssize_t count;
    if (writing) {
        Vec *v = args->a[index].vec;
        unsigned char *data = lls_ipc_payload(c, op, v);
        if (!data) return lls_ipc_nil();
        if (shared) {
            if (offset > st.st_size || v->n > st.st_size - offset)
                return lls_ipc_invalid(c, op, "write would extend the shared memory object");
            do { count = pwrite(fd, data, (size_t)v->n, offset); } while (count < 0 && errno == EINTR);
        } else count = lls_ipc_stream_write(fd, data, (size_t)v->n);
        if (count < 0) return lls_ipc_error(c, op, errno);
        return vint((int64_t)count);
    }
    int64_t length = args->a[index].i;
    if (length < 0 || length > LLS_IPC_LIMIT)
        return lls_ipc_invalid(c, op, "read size must be in 0..1048576");
    unsigned char *buffer = (unsigned char *)ca(c, (size_t)length);
    do { count = shared ? pread(fd, buffer, (size_t)length, offset)
                        : read(fd, buffer, (size_t)length); }
    while (count < 0 && errno == EINTR);
    if (count < 0) return lls_ipc_error(c, op, errno);
    return lls_ipc_bytes(c, buffer, (int)count);
}

#if defined(__linux__)
static Val lls_ipc_mq(Context *c, const char *op, Vec *args) {
    if (!strcmp(op, "mqCreate") || !strcmp(op, "mqOpen") || !strcmp(op, "mqUnlink")) {
        const char *name = args->a[0].s;
        if (!lls_ipc_posix_name(name))
            return lls_ipc_invalid(c, op, "expected a POSIX name of the form /name");
        if (!strcmp(op, "mqUnlink")) {
            if (mq_unlink(name) < 0) return lls_ipc_error(c, op, errno);
            return lls_ipc_nil();
        }
        int create = !strcmp(op, "mqCreate");
        int flags = O_RDWR | O_CLOEXEC | (truth(args->a[args->n - 1]) ? O_NONBLOCK : 0);
        mqd_t fd;
        if (create) {
            int64_t depth = args->a[1].i, size = args->a[2].i;
            if (depth < 1 || depth > INT_MAX || size < 1 || size > LLS_IPC_LIMIT)
                return lls_ipc_invalid(c, op, "queue depth must be in 1..INT_MAX and message size in 1..1048576");
            struct mq_attr attr = {0};
            attr.mq_maxmsg = (long)depth;
            attr.mq_msgsize = (long)size;
            fd = mq_open(name, flags | O_CREAT | O_EXCL, (mode_t)0600, &attr);
        } else fd = mq_open(name, flags);
        if (fd == (mqd_t)-1) return lls_ipc_error(c, op, errno);
        return vint((int64_t)fd);
    }
    int64_t handle = args->a[0].i;
    if (handle < 0 || handle > INT_MAX)
        return lls_ipc_invalid(c, op, "invalid queue descriptor");
    mqd_t fd = (mqd_t)handle;
    if (!strcmp(op, "mqClose")) {
        if (mq_close(fd) < 0) return lls_ipc_error(c, op, errno);
        return lls_ipc_nil();
    }
    if (!strcmp(op, "mqSend")) {
        int64_t priority = args->a[2].i;
        if (priority < 0 || priority > INT_MAX)
            return lls_ipc_invalid(c, op, "priority must be in 0..INT_MAX");
        Vec *v = args->a[1].vec;
        unsigned char *data = lls_ipc_payload(c, op, v);
        if (!data) return lls_ipc_nil();
        int status;
        do { status = mq_send(fd, (const char *)data, (size_t)v->n, (unsigned)priority); }
        while (status < 0 && errno == EINTR);
        if (status < 0) return lls_ipc_error(c, op, errno);
        return lls_ipc_nil();
    }
    struct mq_attr attr;
    if (mq_getattr(fd, &attr) < 0) return lls_ipc_error(c, op, errno);
    if (attr.mq_msgsize < 1 || attr.mq_msgsize > LLS_IPC_LIMIT)
        return lls_ipc_invalid(c, op, "queue maximum message size must be in 1..1048576");
    char *buffer = (char *)ca(c, (size_t)attr.mq_msgsize);
    ssize_t length;
    do { length = mq_receive(fd, buffer, (size_t)attr.mq_msgsize, NULL); }
    while (length < 0 && errno == EINTR);
    if (length < 0) return lls_ipc_error(c, op, errno);
    return lls_ipc_bytes(c, (const unsigned char *)buffer, (int)length);
}

/* Opaque process-local handles keep DSL integers from being dereferenced as
   pointers. Preserve handle identity across slot reuse, and serialize access
   to each reference so a concurrent close cannot unmap it during trywait. */
#define LLS_IPC_SEM_LIMIT 1024
static struct {
    int64_t handle;
    sem_t *semaphore;
} lls_ipc_semaphores[LLS_IPC_SEM_LIMIT];
static pthread_mutex_t lls_ipc_sem_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t lls_ipc_next_semaphore = -1;

static Val lls_ipc_sem(Context *c, const char *op, Vec *args) {
    if (!strcmp(op, "semCreate") || !strcmp(op, "semOpen") || !strcmp(op, "semUnlink")) {
        const char *name = args->a[0].s;
        if (!lls_ipc_posix_name(name))
            return lls_ipc_invalid(c, op, "expected a POSIX name of the form /name");
        if (!strcmp(op, "semUnlink")) {
            if (sem_unlink(name) < 0) return lls_ipc_error(c, op, errno);
            return lls_ipc_nil();
        }
        int create = !strcmp(op, "semCreate");
        int64_t value = create ? args->a[1].i : 0;
        if (value < 0 || value > INT_MAX)
            return lls_ipc_invalid(c, op, "initial semaphore value must be in 0..INT_MAX");
        int status = pthread_mutex_lock(&lls_ipc_sem_lock);
        if (status) return lls_ipc_error(c, op, status);
        int slot = 0;
        while (slot < LLS_IPC_SEM_LIMIT && lls_ipc_semaphores[slot].handle) ++slot;
        if (slot == LLS_IPC_SEM_LIMIT || lls_ipc_next_semaphore == INT64_MIN) {
            pthread_mutex_unlock(&lls_ipc_sem_lock);
            return lls_ipc_invalid(c, op, "semaphore handle limit reached");
        }
        sem_t *semaphore = create ? sem_open(name, O_CREAT | O_EXCL, (mode_t)0600, (unsigned)value)
                                  : sem_open(name, 0);
        int saved = errno;
        if (semaphore == SEM_FAILED) {
            pthread_mutex_unlock(&lls_ipc_sem_lock);
            if (create && saved == ENOENT)
                return lls_ipc_unavailable(c, op, "cannot create the named semaphore; check the host namespace (/dev/shm)");
            return lls_ipc_error(c, op, saved);
        }
        int64_t handle = lls_ipc_next_semaphore--;
        lls_ipc_semaphores[slot].handle = handle;
        lls_ipc_semaphores[slot].semaphore = semaphore;
        pthread_mutex_unlock(&lls_ipc_sem_lock);
        return vint(handle);
    }
    int trying = !strcmp(op, "semTryWait");
    int closing = !strcmp(op, "semClose");
    int64_t handle = args->a[0].i;
    if (handle >= 0) return lls_ipc_invalid(c, op, "invalid or closed semaphore handle");
    int status = pthread_mutex_lock(&lls_ipc_sem_lock);
    if (status) return lls_ipc_error(c, op, status);
    int slot = 0;
    while (slot < LLS_IPC_SEM_LIMIT && lls_ipc_semaphores[slot].handle != handle) ++slot;
    if (slot == LLS_IPC_SEM_LIMIT) {
        pthread_mutex_unlock(&lls_ipc_sem_lock);
        return lls_ipc_invalid(c, op, "invalid or closed semaphore handle");
    }
    sem_t *semaphore = lls_ipc_semaphores[slot].semaphore;
    if (trying) {
        do { status = sem_trywait(semaphore); } while (status < 0 && errno == EINTR);
    } else if (closing) {
        status = sem_close(semaphore);
        if (!status) {
            lls_ipc_semaphores[slot].handle = 0;
            lls_ipc_semaphores[slot].semaphore = NULL;
        }
    } else status = sem_post(semaphore);
    int saved = errno;
    pthread_mutex_unlock(&lls_ipc_sem_lock);
    if (status < 0 && !(trying && saved == EAGAIN)) return lls_ipc_error(c, op, saved);
    return trying ? vbool(status == 0) : lls_ipc_nil();
}
#endif

#endif /* LLS_IPC_POSIX */

static Val lls_ipc_call(Context *c, const char *op, Vec *args) {
    if (!strcmp(op, "available")) {
        if (args->n != 1 || args->a[0].tag != VSTR)
            return lls_ipc_invalid(c, op, "expected one mechanism String");
        const char *family = args->a[0].s;
        int linux_only = !strcmp(family, "mq") || !strcmp(family, "shm") || !strcmp(family, "sem");
        if (!linux_only && strcmp(family, "pipe") && strcmp(family, "fifo") &&
            strcmp(family, "socketpair") && strcmp(family, "poll"))
            return lls_ipc_invalid(c, op, "unknown IPC mechanism");
#if defined(LLS_IPC_POSIX) && defined(__linux__)
        return vbool(1);
#elif defined(LLS_IPC_POSIX)
        return vbool(!linux_only);
#else
        return vbool(0);
#endif
    }
#ifndef LLS_IPC_POSIX
    return lls_ipc_unavailable(c, op, "this build has no POSIX IPC backend");
#else
    if (0) {}
#define LLS_IPC_OP(name, sig, body) \
    else if (!strcmp(op, name)) { \
        if (!lls_ipc_signature(args, sig)) return lls_ipc_invalid(c, op, "invalid argument types"); \
        body; \
    }
    LLS_IPC_OP("pipe", "", return lls_ipc_pair(c, op, 0))
    LLS_IPC_OP("socketPair", "", return lls_ipc_pair(c, op, 1))
    LLS_IPC_OP("fifoCreate", "s", return lls_ipc_fifo(c, op, args))
    LLS_IPC_OP("fifoOpen", "sb", return lls_ipc_fifo(c, op, args))
    LLS_IPC_OP("fifoUnlink", "s", return lls_ipc_fifo(c, op, args))
#if defined(__linux__)
    LLS_IPC_OP("shmCreate", "si", return lls_ipc_shm_open(c, op, args))
    LLS_IPC_OP("shmOpen", "sb", return lls_ipc_shm_open(c, op, args))
    LLS_IPC_OP("shmUnlink", "s", return lls_ipc_shm_open(c, op, args))
    LLS_IPC_OP("mqCreate", "siib", return lls_ipc_mq(c, op, args))
    LLS_IPC_OP("mqOpen", "sb", return lls_ipc_mq(c, op, args))
    LLS_IPC_OP("mqUnlink", "s", return lls_ipc_mq(c, op, args))
    LLS_IPC_OP("mqClose", "i", return lls_ipc_mq(c, op, args))
    LLS_IPC_OP("mqReceive", "i", return lls_ipc_mq(c, op, args))
    LLS_IPC_OP("mqSend", "ivi", return lls_ipc_mq(c, op, args))
    LLS_IPC_OP("semCreate", "si", return lls_ipc_sem(c, op, args))
    LLS_IPC_OP("semOpen", "s", return lls_ipc_sem(c, op, args))
    LLS_IPC_OP("semTryWait", "i", return lls_ipc_sem(c, op, args))
    LLS_IPC_OP("semPost", "i", return lls_ipc_sem(c, op, args))
    LLS_IPC_OP("semClose", "i", return lls_ipc_sem(c, op, args))
    LLS_IPC_OP("semUnlink", "s", return lls_ipc_sem(c, op, args))
#else
    else if (!strncmp(op, "mq", 2))
        return lls_ipc_unavailable(c, op, "POSIX message queues require a Linux host");
    else if (!strncmp(op, "shm", 3))
        return lls_ipc_unavailable(c, op, "POSIX shared memory with positioned I/O requires a Linux host");
    else if (!strncmp(op, "sem", 3))
        return lls_ipc_unavailable(c, op, "POSIX named semaphores require a Linux host");
#endif
    const char *sig = !strcmp(op, "read") ? "ii" : !strcmp(op, "write") ? "iv" :
                      !strcmp(op, "close") || !strcmp(op, "shmSize") ? "i" :
                      !strcmp(op, "setNonblocking") ? "ib" :
                      !strcmp(op, "waitReadable") || !strcmp(op, "waitWritable") ? "ii" :
                      !strcmp(op, "shmRead") ? "iii" : !strcmp(op, "shmWrite") ? "iiv" : NULL;
#undef LLS_IPC_OP
    if (!sig) return lls_ipc_invalid(c, op, "unknown IPC operation");
    if (!lls_ipc_signature(args, sig)) return lls_ipc_invalid(c, op, "invalid argument types");
    int64_t handle = args->a[0].i;
    if (handle < 0 || handle > INT_MAX) return lls_ipc_invalid(c, op, "invalid file descriptor");
    int fd = (int)handle;
    if (!strcmp(op, "close")) {
        if (close(fd) < 0) return lls_ipc_error(c, op, errno);
        return lls_ipc_nil();
    }
    if (!strcmp(op, "setNonblocking")) {
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0 ||
            fcntl(fd, F_SETFL, truth(args->a[1]) ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) < 0)
            return lls_ipc_error(c, op, errno);
        return lls_ipc_nil();
    }
    if (!strcmp(op, "waitReadable") || !strcmp(op, "waitWritable"))
        return lls_ipc_wait(c, op, fd, !strcmp(op, "waitReadable") ? POLLIN : POLLOUT, args->a[1].i);
    return lls_ipc_io(c, op, fd, args);
#endif
}
