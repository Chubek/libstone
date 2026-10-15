module gateml.ipc.FIFO;
import gateml.AST;
import gateml.ipc.Posix;
import core.sys.posix.sys.stat;
import core.sys.posix.fcntl;
import core.sys.posix.unistd;
import std.path : absolutePath;

/// Creating is exclusive. Opening an existing FIFO never grants unlink ownership.
class FIFO {
    private char[4096] path;
    private PathIdentity identity;
    private bool owner;
    private pid_t creator;
    this(string filename, bool create = true) {
        checkPath(filename); copyCString(absolutePath(filename), path[]);
        if (create) {
            if (mkfifo(path.ptr, 0x180) < 0) systemError("mkfifo"); // 0600
            owner = true;
            creator = getpid();
            identity = PathIdentity(path.ptr);
        } else {
            stat_t info;
            if (lstat(path.ptr, &info) < 0) systemError("stat FIFO");
            if (!S_ISFIFO(info.st_mode)) throw new GateError("path is not a FIFO");
        }
    }
    FileDescriptor openReader(bool nonblocking = true) { return openEnd(O_RDONLY, nonblocking); }
    FileDescriptor openWriter(bool nonblocking = true) { return openEnd(O_WRONLY, nonblocking); }
    private FileDescriptor openEnd(int mode, bool nonblocking) {
        auto fd = open(path.ptr, mode | O_NOFOLLOW | O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0));
        if (fd < 0) systemError("open FIFO");
        scope(failure) core.sys.posix.unistd.close(fd);
        stat_t info;
        if (fstat(fd, &info) < 0) systemError("fstat FIFO");
        if (!S_ISFIFO(info.st_mode)) throw new GateError("path is not a FIFO");
        return new FileDescriptor(fd);
    }
    void close() nothrow @nogc {
        if (owner) { if (creator == getpid() && identity.matches(path.ptr)) unlink(path.ptr); owner = false; }
    }
    ~this() { close(); }
}
