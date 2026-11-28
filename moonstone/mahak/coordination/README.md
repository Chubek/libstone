# Mahak development coordination layer

This directory is **development infrastructure**, not part of Mahak's measured
execution path. `Moonstone::Mahak` never links or calls it. It is built only
when `MOONSTONE_BUILD_MAHAK_COORDINATION=ON`.

It implements the eight-worker development protocol:

- Worker 1: orchestrator.
- Worker 2: broker.
- Workers 3–8: implementation territories.
- Every worker has an inbound POSIX message queue.
- The broker has an ingress queue.
- All point-to-point traffic goes through the broker.
- Shared institutional memory is a blotter under
  `/dev/shm/mahak-<session-id>/`.
- Final snapshots are copied outside `/dev/shm` before cleanup.

## Build and test

```sh
cmake -S moonstone -B build-mahak \
  -DMOONSTONE_BUILD_MAHAK=ON \
  -DMOONSTONE_BUILD_MAHAK_COORDINATION=ON \
  -DMOONSTONE_BUILD_TESTS=ON
cmake --build build-mahak --parallel
ctest --test-dir build-mahak -R moonstone.mahak.coordination --output-on-failure
build-mahak/mahak/coordination/mahak-coord-demo --snapshot moonstone/mahak/dev-sessions
```

`mahak-coord-demo` forks one broker and seven participant processes (the parent
is the orchestrator). That is eight simultaneously operating worker roles. It
exercises CONTRACT, REQUEST, RESPONSE, ACK, BLOCKED, STATUS, REVIEW, ERROR and
COMPLETE messages, duplicate suppression, bounded-queue backpressure, an
unresponsive worker diagnosis, and orderly shutdown. It copies the blotter to
`<snapshot>/<session>/` and writes `DEMO-SUMMARY.json`.

## Important host constraint: mqueue memory

The build host used for this implementation accounts **all** message queues
against a small budget (empirically 8 KiB for the whole user namespace, despite
`RLIMIT_MSGQUEUE` reporting 819200 bytes). Direct experiments:
`9 x (1 slot x 512 B)` succeeds; `8 x (1 slot x 1024 B)` fails with `EMFILE`.
The layer therefore uses:

- one slot of 512 bytes per queue,
- nine queues per session (broker + eight workers),
- a per-queue message-size limit of 480 bytes,
- a blotter-backed **spool** for larger payloads: the queue message contains
  `{"$spool":"spool/<id>.json"}` and the receiver resolves it transparently.

This is real POSIX mqueue traffic, not a simulation. The spool is an explicit
adaptation to a measured kernel/namespace constraint, documented in
`PROTOCOL.md`.

## Files

| File | Role |
| --- | --- |
| `include/mahak/coordination/Protocol.hpp` | envelope, message types, ids |
| `include/mahak/coordination/Mailbox.hpp` | bounded POSIX mqueue wrapper |
| `include/mahak/coordination/Blotter.hpp` | shared memory, locks, journal, spool |
| `include/mahak/coordination/Broker.hpp` | routing, dedup, heartbeats, shutdown |
| `include/mahak/coordination/Participant.hpp` | worker endpoint |
| `src/` | implementations |
| `tools/broker.cpp` | standalone broker |
| `tools/status.cpp` | inspect/snapshot a blotter |
| `tools/demo.cpp` | eight-role protocol exercise |
| `tests/protocol.cpp` | envelope, mailbox, blotter concurrency tests |
| `PROTOCOL.md` | wire and lifecycle contract |
