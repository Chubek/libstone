# Coordination protocol

## Worker roster

`/dev/shm/foo-blotter/WORKERS.json` is written once by the orchestrator before
the startup barrier and is read-only for everyone else. It is the only place a
comrade name is defined, so a worker never needs to know another worker's
numeric id in advance.

```json
{
  "v": 1,
  "revision": 1,
  "session": "0123456789abcdef",
  "root": "/dev/shm/foo-blotter",
  "broker_ingress": "/mahak-0123456789abcdef-broker",
  "published_unix_ns": 1760000000000000000,
  "workers": [
    {"name": "Atlas", "role": "orchestrator", "protocol_id": 1,
     "inbox": "/mahak-0123456789abcdef-broker", "orchestrator": true},
    {"name": "Flint", "role": "engine", "protocol_id": 2,
     "inbox": "/mahak-0123456789abcdef-w2", "orchestrator": false}
  ]
}
```

- Exactly one entry is the orchestrator; its `inbox` is the broker ingress.
- Every other `inbox` is `/mahak-<session>-w<protocol_id>`.
- Publishing is atomic (temp, fsync, rename, directory fsync) under
  `roster.lock`; `session.lock` is the orchestrator's ownership lock for the
  directory and is a different file so publishing never self-deadlocks.
- A revised roster increments `revision`. The broker re-reads the file when its
  mtime changes, so name resolution follows a revision without a restart.
- `ACKS.json` records `{"v":1,"revision":N,"acked":{name: unix_ns}}`. An
  acknowledgement for any revision other than the published one is rejected:
  a worker cannot claim a roster it never read. A revision bump clears every
  previous acknowledgement.

## Named addressing

A recipient may be a comrade name, a numeric protocol id, `broker`, or `*`.
The broker resolves a name through `WORKERS.json` before routing, journals a
`named-route` event (`message_id`, `from`, `comrade`, `protocol_id`), and
forwards to `w<protocol_id>`. An unknown name is rejected with an `ERROR`
envelope whose message names the comrade; it is never silently dropped.

## Addressing

- Worker mailbox: `/mahak-<session>-w<worker-id>`.
- Broker ingress: `/mahak-<session>-broker`.
- `<worker-id>` is `1`…`8`; `broker` and `*` are reserved recipient values.
- `<session>` is a 16-hex-character random id. It appears in the blotter path
  `/dev/shm/mahak-<session>/` and in every queue name.

Workers create and own their inbound queue. The broker attaches lazily when
routing and unlinks worker queues during orderly shutdown. A worker unlinks a
stale queue with its own name before creating it; session ids prevent accidental
collisions between simultaneous sessions.

## Envelope

Encoded as strict JSON, at most 480 bytes per queue message:

```json
{
  "v": 1,
  "id": "3-17",
  "sender": "3",
  "recipient": "1",
  "sequence": 17,
  "correlation": "3-req-123456789",
  "type": "REQUEST",
  "priority": 5,
  "timestamp_ns": 1234567890123456789,
  "payload": {"op": "request-task", "worker": "3"}
}
```

- `id` is unique per sender session (`<sender>-<sequence>`).
- `sequence` is strictly increasing per sender.
- `correlation` is echoed by RESPONSE and ACK messages.
- `type` is one of `REQUEST`, `RESPONSE`, `ACK`, `BLOCKED`, `CONTRACT`,
  `REVIEW`, `STATUS`, `ERROR`, `COMPLETE`.
- `priority` is 0–31. Responses outrank bulk acknowledgements.
- `payload` is a JSON object.

Broker-generated messages use id `broker-<n>`.

### Spooled payloads

If an encoded envelope would exceed the queue message budget, the sender writes
the payload to `<blotter>/spool/<id>.json` and replaces it with:

```json
{"$spool": "spool/ab12...-7.json"}
```

`Participant::receive` and the broker resolve this transparently. The spool file
is part of the session snapshot and is removed with the session. This preserves
the envelope contract while respecting the host's 8 KiB total queue budget.

## Operations in `payload.op`

| op | Sender→recipient | Effect |
| --- | --- | --- |
| `register` | worker→broker | Records the worker, returns RESPONSE |
| `heartbeat` | worker→broker | Updates liveness; no ACK flood |
| `inspect` | any→broker | Returns worker/pending/request/statistics snapshot |
| `shutdown` | orchestrator→broker | Returns RESPONSE, then orderly shutdown |
| `ack` | receiver→broker | Correlates `ack_for`; broker forwards delivery ACK |
| `verdict` | review worker→broker | Records a verdict in the blotter |
| `status` | worker→orchestrator | Routed progress update |
| `contract` | orchestrator→`*` | Routed broadcast; ACK carries delivery count |
| `complete` | any→any | Normal completion notice |

Unknown local operations return ERROR. Unknown recipients return ERROR to the
sender. Malformed envelopes are rejected before routing and recorded in the
journal with the broker's error path.

## Delivery, duplicates, acknowledgements

- The broker deduplicates on `(sender, sequence)`. A duplicate gets an ACK and
  is not routed again. The broker records the duplicate count.
- On successful forwarding the broker sends a low-priority ACK to the sender
  (`stage="broker-accepted"`).
- The receiver ACKs non-ACK/non-STATUS work items (`stage` is caller-defined);
  the broker forwards a delivery ACK to the original sender
  (`stage="delivery"`).
- ACK and STATUS messages are not themselves ACKed, preventing ACK storms on
  one-slot queues.
- A full recipient queue is backpressure. The broker retries with a bounded
  delay, records `backpressure`, and reports a BLOCKED/ERROR if delivery still
  fails. It never silently drops a message.
- Outstanding deliveries time out after `request_timeout_ms`; the sender gets
  ERROR and the pending record is removed.

## Liveness and failure

- Every worker sends STATUS heartbeat messages. The broker updates
  `last_seen` on any message.
- A worker silent for `heartbeat_timeout_ms` is marked unresponsive in the
  blotter and the orchestrator receives ERROR `{op:"worker-unavailable"}`.
- If every outstanding request's recipient is blocked or unavailable, the broker
  emits a rate-limited ERROR `{op:"deadlock-suspected"}` to worker 1.
- Pending request/response correlations are tracked; `inspect` exposes counts.

## Shutdown

`REQUEST{op:"shutdown"}` (or SIGINT/SIGTERM) starts orderly shutdown:

1. The broker responds to the requester.
2. It sends COMPLETE to every registered worker.
3. It drains acknowledgements for `shutdown_grace_ms`.
4. It records final statistics and worker-2 status in the blotter.
5. It unlinks all session queues and exits.

A worker that leaves sends COMPLETE and updates the blotter. The session driver
copies the blotter outside `/dev/shm` and then removes the session directory.
Stale queues from a crashed session are unlinked by a later session with the
same id only; ids are random, so this does not affect other sessions.

## Blotter layout

```
/dev/shm/mahak-<session>/
  manifest.json              schema, session, owner pid, revision, workers
  journal.ndjson             append-only event history (revisioned)
  decisions.ndjson           orchestrator decisions
  workers/<id>.json          atomically replaced status record
  contracts/<name>.json      latest contract
  contracts/<name>.r<N>.json immutable contract revisions
  verdicts/<id>.json         immutable Jury verdict records
  spool/<id>.json            out-of-line payloads
  session.lock               exclusive mutation flock
```

All writes use temp-file + fsync + rename under `session.lock`. The journal is
the revision history; a worker status update never silently discards earlier
values. Blotter files are mode 0600 and the session directory mode 0700.
