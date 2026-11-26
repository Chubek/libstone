# ASDL worker coordination protocol (version 1)

`mqctl` transports messages through POSIX message queues. It is a development
tool, not part of the ASDL compiler or its embedding ABI.

```sh
c++ -std=c++20 -D_POSIX_C_SOURCE=200809L tools/mqctl.cpp -o mqctl -lrt
```

On systems that provide the POSIX MQ functions in libc, `-lrt` is unnecessary.
The kernel must permit creation of named message queues.

## Session and queue ownership

Choose a fresh, random session ID of at most 48 ASCII letters, digits, `_` or
`-`. Create one queue per role before launching workers. Names are
`/asdl_<session>_<role>`, where the role is `cpp`, `grammar`, `codegen`, `lua`,
`loader`, `stdlib`, `logistics`, `integration`, or `abi`. Only that role may
receive from the queue. Any session member can send to it. `inspect` reads only
queue attributes; it does not consume a message.

`create` uses exclusive creation with owner-only permissions (`0600`, subject
to umask). Defaults are four messages of at most 2048 bytes each, including
the header. Set `[DEPTH [SIZE]]` explicitly for tighter host limits. If any
creation fails, the coordinator should unlink only the queues it successfully
created and start a new session. This is same-user coordination, not
authentication: POSIX permissions cannot distinguish roles running as the
same user, and `mqctl` does not verify the identity of the sending process.

## Wire frame

```text
ASDL-MQ/1<TAB>sender<TAB>recipient<TAB>id<TAB>correlation<TAB>kind<TAB>priority<LF>
payload bytes (possibly empty)
```

IDs are one to 64 ASCII letters, digits, `_` or `-`. Use unique IDs per
sender, for example `<role>-<sequence>`. Unsolicited messages use correlation
`-`; replies include the original message's ID. Priority is 0–31 and is also
passed to the kernel. The kernel receives higher-priority messages first;
equal-priority messages are FIFO. `receive` checks the version, destination,
and priority and emits the exact frame. Payloads may contain newlines and
NUL bytes.

| Kind | Use |
| --- | --- |
| `REQUEST`, `RESPONSE` | Ask a question or request an action; reply with the outcome. |
| `PROPOSAL`, `ACCEPT`, `REJECT` | Negotiate and acknowledge a shared contract change. |
| `BLOCKED`, `READY` | Report a dependency or signal availability. |
| `REVIEW`, `PATCH` | Request review or submit an implementation artifact. |
| `HEARTBEAT` | Signal liveness, without requiring a reply. |
| `DONE`, `ABORT` | Report a terminal result or request early shutdown. |

For payloads too large for a frame, send a Git commit ID and, if needed, a
repository-relative path (`git:<full-commit-id>:<path>`). Commit and keep the
containing branch until the recipient acknowledges it. Consumers should read
the named object rather than a mutable file in another worker's checkout.

## Backpressure, acknowledgment and shutdown

```sh
./mqctl create "$session" grammar
printf '%s\n' 'Please review the proposed parser interface.' |
  ./mqctl send "$session" grammar cpp cpp-1 - PROPOSAL 0 1000
# Run from the grammar worker only:
./mqctl receive "$session" grammar 1000
```

`send` and `receive` use absolute POSIX deadlines and preserve the same
deadline when interrupted. Status `3` means timeout (no message was sent or
received), `2` means bad arguments or an invalid frame, and `1` means another
failure. Use no more than three send attempts, draining your own inbox or
completing independent work between attempts. Do not hold a lock or wait on
another worker while sending. Priorities do not reserve kernel capacity:
`ABORT` also needs room to enqueue.

A successful send means kernel delivery, **not** application acknowledgment.
Receivers send a correlated `RESPONSE`, `ACCEPT`, or `REJECT` for requests,
proposals, review/patch handoffs, and terminal messages. If a sender retries
after a missing acknowledgment, it keeps the same ID; the receiver should
recognize duplicates by `(sender, id)`, not repeat side effects, and resend
the original acknowledgment.

The coordinator acknowledges each `DONE` and records the worker's final
commit and test results. For cancellation, send `ABORT` to each affected
worker and collect their acknowledgments. If a recipient is unavailable or
its queue remains full after bounded retries, record the role as failed and
stop that worker through the launcher before cleanup. Unlink each queue with
`mqctl cleanup SESSION ROLE` only after its receiver has stopped. Unlinking
is idempotent and does not by itself terminate or wake a process that already
has an open descriptor.
