"""Exercise the real POSIX transport using a private, disposable queue."""

import re
import secrets
import subprocess
import sys


session = "test_" + secrets.token_hex(12)
recipient = "integration"


def call(command, *arguments, payload=b"", status=0):
    proc = subprocess.run(
        [sys.argv[1], command, session, recipient, *arguments],
        input=payload, capture_output=True, timeout=5,
    )
    if status is not None and proc.returncode != status:
        raise RuntimeError(f"{command} {arguments}: want {status}, got {proc!r}")
    return proc


def send(message_id, correlation="-", kind="REQUEST", priority="0", payload=b"", status=0):
    return call("send", "cpp", message_id, correlation, kind, priority, "25",
                payload=payload, status=status)


def frame(message_id, correlation, kind, priority, payload):
    return (f"ASDL-MQ/1\tcpp\t{recipient}\t{message_id}\t{correlation}\t{kind}\t{priority}\n"
            .encode() + payload)


def require(got, expected):
    if got != expected:
        raise RuntimeError(f"expected {expected!r}, got {got!r}")


created = call("create", "3", "256", status=None)
if created.returncode != 0:
    if re.search(rb"Permission denied|Operation not permitted|Function not implemented|"
                 rb"Operation not supported|No space left on device", created.stderr):
        print(f"POSIX queues unavailable: {created.stderr.decode().strip()}")
        sys.exit(77)
    raise RuntimeError(f"queue creation failed: {created!r}")

try:
    queue_name = f"/asdl_{session}_{recipient}\n".encode()
    require(created.stdout, queue_name)
    call("create", status=1)  # Exclusive creation must not reopen another queue.
    call("receive", "10", status=3)
    send("bad.id", status=2)
    send("bad-kind", kind="UNKNOWN", status=2)
    send("oversize", payload=b"x" * 256, status=2)
    payload = b"multiline payload\nwith \x00binary\tdata"
    send("first", payload=payload)
    send("response", "first", "RESPONSE", "7", b"reply\n")
    send("last", payload=b"tail")
    require(call("inspect").stdout, queue_name.rstrip() + b" depth=3 size=256 pending=3\n")
    send("retry", status=3)
    require(call("receive").stdout, frame("response", "first", "RESPONSE", "7", b"reply\n"))
    send("retry", payload=b"retry")
    require(call("receive").stdout, frame("first", "-", "REQUEST", "0", payload))
    require(call("receive").stdout, frame("last", "-", "REQUEST", "0", b"tail"))
    require(call("receive").stdout, frame("retry", "-", "REQUEST", "0", b"retry"))
    call("receive", "0", status=3)
finally:
    call("cleanup")

call("cleanup")  # Idempotent.
call("inspect", status=1)
