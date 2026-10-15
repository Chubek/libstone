module gateml.ipc.Semaphore;
import gateml.AST;
import gateml.ipc.Posix;
import core.sys.posix.semaphore;
import core.sys.posix.fcntl;
import core.stdc.errno;
import core.sys.posix.unistd : getpid, pid_t;

/// Named, process-shared semaphore. Creation is exclusive; only the creator
/// unlinks it. wait retries EINTR; tryWait never blocks.
class Semaphore {
    private sem_t* sem = SEM_FAILED;
    private char[257] name;
    private bool owner;
    private pid_t creator;
    this(string name, uint initial = 0, bool create = true) {
        checkName(name); copyCString(name, this.name[]);
        sem = create ? sem_open(this.name.ptr, O_CREAT | O_EXCL, 0x180, initial) : sem_open(this.name.ptr, 0);
        if (sem == SEM_FAILED) systemError("sem_open");
        owner = create; creator = getpid();
    }
    private void check() { if (sem == SEM_FAILED) throw new GateError("closed semaphore"); }
    void wait() {
        check(); while (sem_wait(sem) < 0) if (errno != EINTR) systemError("sem_wait");
    }
    bool tryWait() {
        check();
        while (sem_trywait(sem) < 0) {
            if (errno == EAGAIN) return false;
            if (errno != EINTR) systemError("sem_trywait");
        }
        return true;
    }
    void post() { check(); if (sem_post(sem) < 0) systemError("sem_post"); }
    void close() nothrow @nogc {
        if (sem != SEM_FAILED) { sem_close(sem); sem = SEM_FAILED; }
        if (owner) { if (creator == getpid()) sem_unlink(name.ptr); owner = false; }
    }
    ~this() { close(); }
}
