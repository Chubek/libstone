# Native IPC in LLSynth

Import `.import std.ipc as IPC` to use the host's local interprocess
communication APIs in generated tool actions. This is not code emitted into
programs by a `.backend`. The library does not invoke shell commands, emulate
IPC, or require an external helper at runtime.

```text
.language "PipeExample"
.import std.ipc as IPC
.rule Start
'syntax: EOF
'semantics:
let ends = IPC:pipe();
discard IPC:write(ends[1], [65, 0, 255]);
discard IPC:close(ends[1]);
let bytes = IPC:read(ends[0], 16);
discard IPC:close(ends[0]);
return bytes[2];
.end
```

On Linux, either of these generated tools prints `255` for empty input:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T python3 -o tool.py pipe.llsynth
python3 tool.py </dev/null
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T c11 -o tool.c pipe.llsynth
cc -std=c11 tool.c -pthread -lrt -lm -o tool
./tool </dev/null
```

The Python target uses only its standard library and native host APIs.
Generated C99/C11 tools embed the IPC runtime; with CMake, link the generated
target privately to `LLSynth::ipc` for its extra host link requirements.
Use `-pthread` for POSIX thread-local signal masks and semaphore support, and
`-lrt` on Linux for compatibility with older libc releases. Tools that do
not import `std.ipc` do not require these IPC dependencies. Generated C++
tools do not support native interop.

## Operations

Parameters named `fd`, `queue` or `semaphore`, as well as sizes, offsets,
timeouts and priorities, are `Int`; flags are `Bool`; paths and names are
`String`; payloads are `Vec<Int>` octet vectors (`0..255`, including NUL).
The library bounds one transfer to 1 MiB. Return types are shown below.

| API | Result and behavior |
| --- | --- |
| `available(mechanism)` | `Bool`: reports the presence of the platform/build API for `"pipe"`, `"fifo"`, `"socketpair"`, `"poll"`, `"mq"`, `"shm"`, or `"sem"`. Not a probe of kernel configuration, permissions, mounts, or quotas. |
| `pipe()` | `Vec<Int>`: owned `[readFd, writeFd]` for a blocking anonymous byte stream. |
| `socketPair()` | `Vec<Int>`: two owned, connected, blocking, full-duplex Unix-domain stream socket descriptors. |
| `close(fd)` | `Void`: releases one owned descriptor. Do not repeat a close or retry a failed close with the same integer. Use `mqClose` for queues. |
| `read(fd, count)` | `Vec<Int>`: up to `count` bytes. May be short; `[]` means EOF when `count > 0`. |
| `write(fd, data)` | `Int`: bytes written, possibly fewer than supplied. A closed peer is an error, not process termination via `SIGPIPE`. |
| `setNonblocking(fd, enabled)` | `Void`: changes the status of the underlying open file description, also affecting any duplicates. |
| `waitReadable(fd, milliseconds)`, `waitWritable(fd, milliseconds)` | `Bool`: POSIX `poll` readiness; `false` on timeout. Wait from `0` (test immediately) to `2147483647` ms. A hangup or pending error counts as ready; perform the I/O to discover EOF or the error. An invalid descriptor raises an error. |
| `fifoCreate(path)` | `Void`: creates a FIFO with owner read/write permissions (`0600`, subject to umask). Fails rather than replacing an existing path. |
| `fifoOpen(path, writable)` | `Int`: opens an existing FIFO nonblocking in one direction (read if `false`, write if `true`). Open the reader first; opening a nonblocking writer without a reader reports `ENXIO`. Rejects a final-component symlink or a non-FIFO. |
| `fifoUnlink(path)` | `Void`: removes a FIFO's directory entry, not its already-open descriptors. Use paths in a directory you or your peers control: the portable type check followed by unlink is not atomic against path replacement. |
| `mqCreate(name, depth, messageSize, nonblocking)` | `Int`: creates and opens a Linux POSIX message queue in read/write mode, `0600` subject to umask. Name must be unused. Positive depth and maximum message size also obey the kernel's limits. |
| `mqOpen(name, nonblocking)` | `Int`: opens an existing queue in read/write mode. |
| `mqSend(queue, data, priority)` | `Void`: sends one complete message, including zero-length messages. Priority is nonnegative and bounded by the host; higher values are delivered first. |
| `mqReceive(queue)` | `Vec<Int>`: receives one whole message; receives the highest priority first, with FIFO order among equal priorities. Does not return priority. A zero-length message is not EOF. |
| `mqClose(queue)`, `mqUnlink(name)` | `Void`: releases the queue descriptor or removes the name, respectively. Unlink does not immediately close outstanding descriptors. |
| `shmCreate(name, size)` | `Int`: exclusively creates a POSIX shared memory object, opens read/write, and sizes it to `0..1073741824` bytes. Mode is `0600`, subject to umask. Signal peers after this operation completes to avoid racing initial sizing. |
| `shmOpen(name, writable)` | `Int`: opens an existing shared memory object read-only (`false`) or read/write (`true`). |
| `shmSize(fd)` | `Int`: object's current size in bytes. |
| `shmRead(fd, offset, count)` | `Vec<Int>`: copies at most `count` bytes from a nonnegative offset without changing the file cursor. Short reads are possible, with `[]` at or beyond the end. |
| `shmWrite(fd, offset, data)` | `Int`: copies bytes to the object without moving the cursor or intentionally growing the object. Rejects out-of-bounds writes; return value may be short. |
| `shmUnlink(name)` | `Void`: removes the name; call `close(fd)` separately. |
| `semCreate(name, initialValue)` | `Int`: exclusively creates and opens a Linux POSIX named semaphore with `0..2147483647` initial tokens. Mode is `0600`, subject to umask. |
| `semOpen(name)` | `Int`: opens an existing named semaphore. Each call returns a separate handle even when it refers to the same underlying semaphore. |
| `semTryWait(semaphore)` | `Bool`: atomically consumes one token and returns `true`, or returns `false` without waiting for a token if none is available. Other failures are errors. |
| `semPost(semaphore)` | `Void`: adds one token, or reports an error if the count would overflow. |
| `semClose(semaphore)`, `semUnlink(name)` | `Void`: releases one handle or removes the name, respectively. Existing handles remain valid after unlink. |

The POSIX names for queues, shared memory, and semaphores have the form
`/name` with a single leading slash, no other slashes, and a nonempty
basename other than `.` or `..`. With Linux's usual POSIX shared memory
mount, `shmCreate("/name", n)` creates `/dev/shm/name`. Clients outside
LLSynth may map the same object; the library never exposes mapped pointers.
When it is safe to read, write or resize belongs to the application's protocol.

## Availability, failures, and ownership

* The runtime uses POSIX hosts for pipes, FIFOs, socket pairs, and polling.
  POSIX message queues, shared-memory positioned I/O, and named semaphores
  are Linux-only in this implementation. Linux is the exercised host; other
  POSIX hosts depend on their libc/Python facilities.
* A disabled or unavailable API reports `IPC_UNAVAILABLE`; an available
  API can still fail at runtime because of permissions, quotas or host
  configuration. C can explicitly compile a stub with `-DLLS_IPC_DISABLE`.
* `IPC_WOULD_BLOCK` is the error for `EAGAIN` or `EWOULDBLOCK`. Invalid
  arguments and other host failures report `IPC`; host errors include the
  operation, OS error text, and numeric `errno`. The library neither retries
  a would-block condition nor silently supplies an empty payload on failure.
* Stream operations and queue transfers can block. Use nonblocking modes and
  readiness checks when a blocking transfer is not appropriate. Read/write,
  queue transfers and readiness checks retry ordinary `EINTR`; readiness
  without an event preserves the original timeout. An interrupted close is
  never retried. There is no automatic cancellation or implicit buffering.
* Descriptors are integer values under caller ownership, not garbage-collected
  or closed on a context reset, action error, or task completion. They can
  be reused by the OS after close. Use only owned descriptors, or intentionally
  shared/inherited descriptors whose lifetime is coordinated by the caller.
  In particular, do not close a descriptor while another thread is using it.
* Semaphore handles are opaque integer tokens, not descriptors or native
  addresses. Use `semClose`, not `close`. Each generated component can hold
  up to 1024 open semaphore handles at once. Handles are not reset with a
  context and cannot be passed to another process or generated component;
  exchange the name and open it in each process instead. These operations
  are not async-signal-safe.
* New descriptors are close-on-exec (on platforms without atomic creation for
  unnamed pairs, setting this flag is a separate operation). Named objects
  persist until unlinked, so arrange cleanup separately if an action fails.
  No operation supplies authentication, serialization, or synchronization
  for protocol data beyond the documented host primitive.

Run `python3 cornerstone/LLSynth/tests/stdlib/test_ipc.py` for generated Python
and C round trips, cross-process IPC, error paths, and unavailable-platform
behavior.
