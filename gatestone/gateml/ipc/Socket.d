module gateml.ipc.Socket;
import gateml.AST;
import gateml.ipc.Posix;
import gateml.serde.Binary;
import core.sys.posix.sys.socket;
import core.sys.posix.sys.un;
import core.sys.posix.unistd;
import core.sys.posix.sys.stat : chmod;
import core.stdc.errno;
import std.path : absolutePath;

// Linux socket creation flag, absent from Druntime's POSIX-only declarations.
private enum SOCK_CLOEXEC = 0x80000;

private sockaddr_un address(string path) {
    checkPath(path); sockaddr_un addr; addr.sun_family = AF_UNIX;
    if (path.length >= addr.sun_path.length) throw new GateError("Unix socket path too long");
    addr.sun_path[0 .. path.length] = cast(const byte[]) path;
    return addr;
}

/// Blocking Unix stream socket with length-prefixed binary messages. Each
/// endpoint must have a single writer/reader or external synchronization.
class Socket : FileDescriptor {
    enum maxMessage = 16 * 1024 * 1024;
    this(int fd) { super(fd); }
    override void writeAll(const(ubyte)[] bytes) {
        while (bytes.length) {
            auto count = send(handle, bytes.ptr, bytes.length, MSG_NOSIGNAL);
            if (count < 0) { if (errno == EINTR) continue; systemError("socket send"); }
            if (!count) throw new GateError("zero-length socket send");
            bytes = bytes[cast(size_t) count .. $];
        }
    }
    static Socket connect(string path) {
        auto addr = address(path); auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) systemError("socket");
        scope(failure) core.sys.posix.unistd.close(fd);
        if (core.sys.posix.sys.socket.connect(fd, cast(sockaddr*) &addr, cast(socklen_t) addr.sizeof) < 0) systemError("connect");
        return new Socket(fd);
    }
    static Socket[2] pair() {
        int[2] ends;
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) < 0) systemError("socketpair");
        Socket[2] sockets;
        scope(failure) foreach (i; 0 .. 2) {
            if (sockets[i] is null) core.sys.posix.unistd.close(ends[i]); else sockets[i].close();
        }
        sockets[0] = new Socket(ends[0]); sockets[1] = new Socket(ends[1]);
        return sockets;
    }
    void sendMessage(const(ubyte)[] bytes) {
        if (bytes.length > maxMessage) throw new GateError("socket message exceeds limit");
        Writer writer; writer.be(bytes.length, 4); writer.raw(bytes);
        writeAll(writer.data);
    }
    ubyte[] receiveMessage() {
        ubyte[4] header; readExact(header[]); Reader reader = Reader(header[]);
        auto size = reader.be(4);
        if (size > maxMessage) throw new GateError("socket message exceeds limit");
        auto bytes = new ubyte[cast(size_t) size]; readExact(bytes); return bytes;
    }
}

class SocketListener : FileDescriptor {
    private char[4096] path;
    private PathIdentity identity;
    private bool owner;
    private pid_t creator;
    this(string filename, int backlog = 16) {
        auto addr = address(filename);
        super(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        scope(failure) close();
        copyCString(absolutePath(filename), path[]);
        // Never remove an existing socket to make bind succeed.
        if (bind(handle, cast(sockaddr*) &addr, cast(socklen_t) addr.sizeof) < 0) systemError("bind");
        owner = true;
        creator = getpid();
        identity = PathIdentity(path.ptr);
        if (chmod(path.ptr, 0x180) < 0) systemError("chmod socket");
        if (listen(handle, backlog) < 0) systemError("listen");
    }
    Socket accept() {
        while (true) {
            auto fd = core.sys.posix.sys.socket.accept(handle, null, null);
            if (fd >= 0) {
                import core.sys.posix.fcntl;
                if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) { core.sys.posix.unistd.close(fd); systemError("accepted socket close-on-exec"); }
                scope(failure) core.sys.posix.unistd.close(fd);
                return new Socket(fd);
            }
            if (errno != EINTR) systemError("accept");
        }
    }
    override void close() nothrow @nogc {
        super.close(); if (owner) { if (creator == getpid() && identity.matches(path.ptr)) unlink(path.ptr); owner = false; }
    }
    ~this() { close(); }
}
