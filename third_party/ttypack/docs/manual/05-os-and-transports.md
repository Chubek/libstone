\page manual_os Operating System and Transports

# Operating System and Transports

## POSIX terminal lifecycle

The OS-facing TTyTk headers are deliberately explicit:

- `Winsize` reads and updates rows, columns, and pixel dimensions;
- `SignalTools` turns signals such as SIGWINCH into application events;
- `PsuedoTerm` wraps PTY creation, fork, read, write, and resize;
- `JobControl` manages process groups and foreground ownership;
- `FlowControl` handles throttling, backpressure, and XON/XOFF;
- `SerialPort` configures hardware terminals;
- `IPCTools` provides local process-to-process channels.

A full-screen application should establish terminal mode, install signal
handling, create or attach its input/output descriptors, and restore state on
all exit paths. Keep restoration separate from rendering so errors do not
leave the user's terminal in an alternate mode.

Signal callbacks are deferred. After registering one, watch
`ttytk::signals::notification_fd()` for readability and call
`ttytk::signals::dispatch_pending()` on the thread that owns your terminal
state. Multiple deliveries of the same signal can be coalesced; a resize
callback should read the latest size rather than count notifications.

Closing a PTY descriptor does not reap its child. Call `PTY::wait()` when
the child exits, or reap `PTY::child_pid()` through your own process manager.
An IPC listener does not unlink its filesystem socket path on destruction;
the application owns that cleanup.

## Shells and PTYs

Use `PsuedoTerm` when the child must believe it is connected to a terminal.
Use TTyUtils `Shell` for command orchestration where captured pipes or
pipelines are sufficient. Use TTyUtils `Terminal` when the child output must
be interpreted and displayed as a terminal session.

`MuxTools` and `EmbedTools` compose PTYs into multiplexed or nested
applications. They should remain at the boundary: the screen model still
belongs to TTyTk and the application policy belongs to TTyUtils.

## Network and local communication

TTyTk supplies synchronous TCP and local IPC primitives through
`ServerNetUtils` and `P2PNetUtils`. TTyUtils exposes transport building blocks
through `NetLink`, `Telnet`, `Serial`, and `P2P`. The dependency-free build
does not implement TLS or SSH; a TCP connection is not a secure fallback.

The higher-level remote stack is:

```text
NetLink
  ├── SSH / Telnet / Serial
  ├── P2P
  └── Roam / Multiplex / SessionShare
```

Treat authentication, host identity, and remote clipboard access as explicit
policy. The transport layer should report errors and state transitions rather
than silently retrying unsafe operations.

## Event loop boundary

TTyTk does not own a scheduler. Its OS calls may block depending on the
operation and descriptor flags. TTyUtils owns coroutine awaitables and
scheduling in `EventLoop`. A TTyTk caller can use another scheduler by polling
non-blocking descriptors and passing completed input to the same parser or
state object.
