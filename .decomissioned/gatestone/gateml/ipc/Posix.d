module gateml.ipc.Posix;
version (linux) {} else { static assert(false, "GateML IPC currently requires Linux"); }
import gateml.AST;
import core.stdc.errno;
import core.stdc.string : strerror;
import core.sys.posix.unistd;
import core.sys.posix.sys.stat;
import std.string : fromStringz;

void systemError(string operation) {
    auto code = errno;
    throw new GateError(operation ~ ": " ~ fromStringz(strerror(code)).idup);
}

void checkPath(string path) {
    if (!path.length) throw new GateError("empty IPC path");
    foreach (char c; path) if (!c) throw new GateError("NUL in IPC path");
}
void checkName(string name) {
    checkPath(name);
    if (name.length < 2 || name[0] != '/') throw new GateError("POSIX IPC names require a leading slash");
    foreach (char c; name[1 .. $]) if (c == '/') throw new GateError("POSIX IPC names must not contain directories");
}

/// Store a C string inside its owning object, not in a separately GC-managed
/// allocation that may have been finalized when the owner's destructor runs.
void copyCString(string value, char[] buffer) {
    checkPath(value);
    if (value.length >= buffer.length) throw new GateError("IPC name or path too long");
    buffer[0 .. value.length] = value[]; buffer[value.length] = '\0';
}

struct PathIdentity {
    private typeof(stat_t.init.st_dev) device;
    private typeof(stat_t.init.st_ino) inode;
    private bool valid;
    this(const(char)* path) {
        stat_t info;
        if (lstat(path, &info) < 0) systemError("stat owned IPC path");
        device = info.st_dev; inode = info.st_ino; valid = true;
    }
    bool matches(const(char)* path) const nothrow @nogc {
        stat_t info;
        return valid && lstat(path, &info) == 0 && info.st_dev == device && info.st_ino == inode;
    }
}

/// Single owning descriptor. close is idempotent; never retry Linux close on
/// EINTR, since retrying can accidentally close a reused descriptor.
class FileDescriptor {
    private int fd = -1;
    this(int descriptor) {
        if (descriptor < 0) systemError("open descriptor");
        fd = descriptor;
    }
    @property int handle() const {
        if (fd < 0) throw new GateError("closed IPC descriptor");
        return fd;
    }
    void close() nothrow @nogc {
        if (fd >= 0) { core.sys.posix.unistd.close(fd); fd = -1; }
    }
    ~this() { close(); }
    size_t readSome(ubyte[] bytes) {
        if (!bytes.length) return 0;
        while (true) {
            auto count = core.sys.posix.unistd.read(handle, bytes.ptr, bytes.length);
            if (count >= 0) return cast(size_t) count;
            if (errno != EINTR) systemError("read");
        }
    }
    void readExact(ubyte[] bytes) {
        while (bytes.length) {
            auto count = readSome(bytes);
            if (!count) throw new GateError("unexpected IPC EOF");
            bytes = bytes[count .. $];
        }
    }
    /// Pipe/FIFO callers must arrange their own SIGPIPE policy.
    void writeAll(const(ubyte)[] bytes) {
        while (bytes.length) {
            auto count = core.sys.posix.unistd.write(handle, bytes.ptr, bytes.length);
            if (count < 0) { if (errno == EINTR) continue; systemError("write"); }
            if (!count) throw new GateError("zero-length IPC write");
            bytes = bytes[cast(size_t) count .. $];
        }
    }
}
