module gateml.ipc.SharedMemory;
import gateml.AST;
import gateml.ipc.Posix;
import core.sys.posix.sys.mman;
import core.sys.posix.sys.stat;
import core.sys.posix.fcntl;
import core.sys.posix.unistd;

/// Shared byte storage only: callers must coordinate access with a semaphore
/// or another process-shared synchronization primitive. Views expire on close.
class SharedMemory {
    private void* mapping = MAP_FAILED;
    private size_t length;
    private char[257] name;
    private bool owner;
    private pid_t creator;
    this(string name, size_t size, bool create = true) {
        checkName(name);
        if (!size || size > 64 * 1024 * 1024) throw new GateError("invalid shared memory size");
        copyCString(name, this.name[]);
        auto fd = shm_open(this.name.ptr, O_RDWR | O_CLOEXEC | (create ? O_CREAT | O_EXCL : 0), 0x180);
        if (fd < 0) systemError("shm_open");
        scope(exit) core.sys.posix.unistd.close(fd);
        scope(failure) if (create) shm_unlink(this.name.ptr);
        if (create && ftruncate(fd, cast(off_t) size) < 0) systemError("size shared memory");
        stat_t info;
        if (fstat(fd, &info) < 0) systemError("stat shared memory");
        if (info.st_size < 0 || cast(ulong) info.st_size < size) throw new GateError("shared memory object is smaller than requested mapping");
        mapping = mmap(null, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) systemError("mmap shared memory");
        length = size; owner = create; creator = getpid();
    }
    ubyte[] bytes() {
        if (mapping == MAP_FAILED) throw new GateError("closed shared memory");
        return (cast(ubyte*) mapping)[0 .. length];
    }
    void close() nothrow @nogc {
        if (mapping != MAP_FAILED) { munmap(mapping, length); mapping = MAP_FAILED; }
        if (owner) { if (creator == getpid()) shm_unlink(name.ptr); owner = false; }
    }
    ~this() { close(); }
}
