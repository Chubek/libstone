module gateml.ipc.MessageQueue;
import gateml.AST;
import gateml.ipc.Posix;
import core.sys.posix.mqueue;
import core.sys.posix.fcntl;
import core.stdc.errno;
import core.sys.posix.unistd : getpid, pid_t;

struct Message { ubyte[] bytes; uint priority; }

/// POSIX named message queue. Nonblocking by default (EAGAIN is a located
/// exception); pass false to opt into blocking send/receive.
class MessageQueue {
    private mqd_t queue = -1;
    private char[257] name;
    private bool owner;
    private pid_t creator;
    private size_t messageSize;
    this(string name, bool create = true, size_t maxMessage = 8192,
         uint capacity = 10, bool nonblocking = true) {
        checkName(name);
        if (!maxMessage || maxMessage > 64 * 1024 * 1024 || !capacity) throw new GateError("invalid queue limits");
        copyCString(name, this.name[]);
        mq_attr attr = mq_attr.init;
        attr.mq_msgsize = cast(typeof(attr.mq_msgsize)) maxMessage;
        attr.mq_maxmsg = capacity;
        auto flags = O_RDWR | O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0);
        queue = create ? mq_open(this.name.ptr, flags | O_CREAT | O_EXCL, 0x180, &attr) : mq_open(this.name.ptr, flags);
        if (queue == -1) systemError("mq_open");
        owner = create; creator = getpid();
        scope(failure) close();
        if (mq_getattr(queue, &attr) < 0) systemError("mq_getattr");
        if (attr.mq_msgsize <= 0 || attr.mq_msgsize > 64 * 1024 * 1024) throw new GateError("invalid queue message size");
        messageSize = cast(size_t) attr.mq_msgsize;
    }
    private void check() { if (queue == -1) throw new GateError("closed message queue"); }
    void send(const(ubyte)[] bytes, uint priority = 0) {
        check();
        if (bytes.length > messageSize) throw new GateError("message exceeds queue limit");
        // mq_send requires a valid pointer even for empty messages.
        const(char)* data = bytes.length ? cast(const(char)*) bytes.ptr : "";
        while (mq_send(queue, data, bytes.length, priority) < 0)
            if (errno != EINTR) systemError("mq_send");
    }
    Message receive() {
        check(); Message result; result.bytes.length = messageSize;
        while (true) {
            auto count = mq_receive(queue, cast(char*) result.bytes.ptr, result.bytes.length, &result.priority);
            if (count >= 0) { result.bytes.length = cast(size_t) count; return result; }
            if (errno != EINTR) systemError("mq_receive");
        }
    }
    void close() nothrow @nogc {
        if (queue != -1) { mq_close(queue); queue = -1; }
        if (owner) { if (creator == getpid()) mq_unlink(name.ptr); owner = false; }
    }
    ~this() { close(); }
}
