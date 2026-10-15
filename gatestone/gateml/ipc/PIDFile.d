module gateml.ipc.PIDFile;
import gateml.AST;
import gateml.ipc.Posix;
import core.sys.posix.fcntl;
import core.sys.posix.unistd;
import core.sys.posix.sys.stat;
import core.sys.linux.sys.file : flock, LOCK_EX, LOCK_NB;
import std.string : toStringz, strip;
import std.conv : to;
import core.stdc.errno : errno, EINTR;

/// Exclusive advisory lock held for the instance's lifetime. The lock file is
/// intentionally retained after close: unlinking a locked inode permits a
/// second process to create a different inode and bypass the lock.
class PIDFile {
    private FileDescriptor file;
    this(string path) {
        checkPath(path);
        auto fd = open(toStringz(path), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0x180);
        if (fd < 0) systemError("open PID file");
        scope(failure) {
            if (file is null) core.sys.posix.unistd.close(fd);
            else file.close();
        }
        stat_t info;
        if (fstat(fd, &info) < 0) systemError("stat PID file");
        if (!S_ISREG(info.st_mode)) throw new GateError("PID path is not a regular file");
        if (flock(fd, LOCK_EX | LOCK_NB) < 0) systemError("lock PID file");
        if (ftruncate(fd, 0) < 0) systemError("truncate PID file");
        file = new FileDescriptor(fd);
        auto pid = getpid().to!string ~ "\n";
        file.writeAll(cast(const ubyte[]) pid);
        if (fsync(fd) < 0) systemError("sync PID file");
    }
    static long readPID(string path) {
        checkPath(path);
        try {
            // Never follow a symlink, block on a FIFO, or read an
            // unbounded file just to obtain a process identifier.
            auto fd = open(toStringz(path), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0) systemError("open PID file");
            scope(exit) core.sys.posix.unistd.close(fd);
            stat_t info;
            if (fstat(fd, &info) < 0) systemError("stat PID file");
            if (!S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > 64)
                throw new GateError("invalid PID file");
            ubyte[65] buffer;
            size_t used;
            while (used < buffer.length) {
                auto count = core.sys.posix.unistd.read(fd, buffer.ptr + used, buffer.length - used);
                if (count < 0) { if (errno == EINTR) continue; systemError("read PID file"); }
                if (!count) break;
                used += cast(size_t) count;
            }
            if (used > 64) throw new GateError("PID file exceeds size limit");
            long pid;
            try { pid = (cast(char[]) buffer[0 .. used]).strip.to!long; }
            catch (Exception error) { throw new GateError("invalid PID file: " ~ error.msg); }
            if (pid <= 0 || pid > pid_t.max) throw new GateError("invalid PID file");
            return pid;
        } catch (GateError error) { error.source = path; throw error; }
    }
    void close() nothrow @nogc { if (file !is null) file.close(); }
    ~this() { close(); }
}
