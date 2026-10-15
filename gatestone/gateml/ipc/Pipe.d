module gateml.ipc.Pipe;
import gateml.ipc.Posix;
import core.sys.posix.unistd;
import core.sys.posix.fcntl;

/// Anonymous unidirectional pipe with independently closeable ends.
class Pipe {
    FileDescriptor reader, writer;
    this() {
        int[2] ends;
        if (pipe(ends) < 0) systemError("pipe");
        scope(failure) {
            if (reader is null) core.sys.posix.unistd.close(ends[0]); else reader.close();
            if (writer is null) core.sys.posix.unistd.close(ends[1]); else writer.close();
        }
        if (fcntl(ends[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(ends[1], F_SETFD, FD_CLOEXEC) < 0) systemError("pipe close-on-exec");
        reader = new FileDescriptor(ends[0]); writer = new FileDescriptor(ends[1]);
    }
    void close() nothrow @nogc {
        if (reader !is null) reader.close();
        if (writer !is null) writer.close();
    }
    ~this() { close(); }
}
