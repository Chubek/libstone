#!/usr/bin/env python3
"""Linux-only development coordinator; not used by LibDSL or its build."""
import argparse
import ctypes as C
import fcntl
import json
import mmap
import os
from pathlib import Path
import uuid

WORKERS = [f"W{i:02}" for i in range(1, 13)]
SIZE = 262144
libc = C.CDLL(None, use_errno=True)
libc.msgsnd.argtypes = [C.c_int, C.c_void_p, C.c_size_t, C.c_int]
libc.msgrcv.argtypes = [C.c_int, C.c_void_p, C.c_size_t, C.c_long, C.c_int]
libc.msgrcv.restype = C.c_ssize_t


class Packet(C.Structure):
    _fields_ = [("route", C.c_long), ("data", C.c_char * 4096)]


def check(result):
    if result < 0:
        error = C.get_errno()
        raise OSError(error, os.strerror(error))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("worker", choices=WORKERS)
    parser.add_argument("action", choices=["init", "send", "recv", "read", "record", "close"])
    parser.add_argument("args", nargs="*")
    opt = parser.parse_args()
    directory, worker, action, args = opt.directory, opt.worker, opt.action, opt.args
    meta = directory / "queue.json"
    shared = directory / "blotter"
    if action in ("init", "close") and worker != "W01":
        parser.error("only W01 manages the coordinator")
    if action == "init":
        directory.mkdir(mode=0o700)
        queue = check(libc.msgget(0, 0o1600))  # IPC_PRIVATE | IPC_CREAT | 0600
        meta.write_text(json.dumps(queue))
        with shared.open("w+b") as stream:
            stream.truncate(SIZE)
        return
    queue = json.loads(meta.read_text())
    if action == "close":
        check(libc.msgctl(queue, 0, None))  # IPC_RMID
    elif action == "send":
        recipient, category, priority, ack, payload, *rest = args
        if recipient not in WORKERS or (worker == "W01") == (recipient == "W01"):
            parser.error("messages must travel between W01 and one worker")
        if ack not in ("yes", "no"):
            parser.error("acknowledgement requirement must be yes or no")
        message = dict(sender=worker, recipient=recipient, category=category, priority=priority,
                       acknowledgement=(ack == "yes"), payload=payload,
                       correlation_id=rest[0] if rest else uuid.uuid4().hex)
        data = json.dumps(message).encode()
        if len(data) >= 4096:
            parser.error("message too large")
        # Each direction has a dedicated route for each worker.
        route = int((recipient if worker == "W01" else worker)[1:]) * 2 + (recipient == "W01")
        packet = Packet(route, data)
        check(libc.msgsnd(queue, C.byref(packet), len(data), 0o4000))
        print(message["correlation_id"])
    elif action == "recv":
        route = (int(args[0][1:]) * 2 + 1) if worker == "W01" else int(worker[1:]) * 2
        packet = Packet()
        size = libc.msgrcv(queue, C.byref(packet), 4096, route, 0o4000)
        if size < 0 and C.get_errno() == 42:  # ENOMSG on Linux
            return
        check(size)
        print(packet.data.decode())
    elif action in ("read", "record"):
        with shared.open("r+b") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX if action == "record" else fcntl.LOCK_SH)
            with mmap.mmap(stream.fileno(), SIZE) as region:
                length = int.from_bytes(region[:4], "little")
                state = json.loads(region[4:4 + length]) if length else {}
                if action == "read":
                    print(json.dumps(state, indent=2))
                    return
                section, payload = args
                if section not in ("contracts", "ownership", "tasks", "blockers", "decisions", "tests", "integration"):
                    parser.error("unknown section")
                if section in ("contracts", "ownership", "decisions", "integration") and worker != "W01":
                    parser.error("reserved for W01")
                log = state.setdefault(section, [])
                log.append(dict(version=len(log) + 1, worker=worker, payload=payload))
                data = json.dumps(state).encode()
                if len(data) + 4 > SIZE:
                    parser.error("blotter full")
                region[:4 + len(data)] = len(data).to_bytes(4, "little") + data


if __name__ == "__main__":
    main()
