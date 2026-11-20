# Oracle integration

The generated Oracle layer lets an IR pipeline offer any macro artifact to a
local, out-of-process transformer. It is an optional optimization hook, not a
required pipeline service: with no endpoint configured, processing is a
successful no-op and does not create a socket, shared-memory object, temporary
file, or annotation sidecar.

Generate the layer with the other IR sources:

```sh
python3 -m archon irgen gated rv32.archdb -o generated
cc -D_POSIX_C_SOURCE=200809L -std=c99 -Wall -Wextra -Werror \
  -pedantic-errors generated/Oracle.c client.c -o client -lrt
```

`Oracle.c` and `Oracle.h` are emitted by default. The generation option
`--option oracle=false` omits both files. `oracle` is the only
generation-time integration key; endpoint paths and limits are runtime
configuration and are never embedded in generated output.

## Artifact and sidecar names

`archon_oracle_process_file` accepts every regular artifact whose final,
case-sensitive suffix is `.i`. It does not use a fixed allowlist such as
`Program.i` or `MIR.i`. `custom output.i`, `.i`, and
`subdirectory/example.i` are valid; `Program`, `Program.I`, and
`Program.i.txt` are not.

For `DIR/NAME.i`, annotations are written to
`DIR/NAME.oracle.i`. Only the final `.i` is replaced, so the sidecar for
`NAME.oracle.i` is `NAME.oracle.oracle.i`. The artifact and sidecar stay in the
artifact's directory so their final installation can use same-filesystem
atomic renames. `archon_oracle_default_annotation_path` computes this name into
a caller-provided buffer.

## Client API and runtime configuration

The public client entry point is:

```c
ArchonOracleStatus archon_oracle_process_file(
    const char *artifact_path,
    ArchonOracleStage stage,
    const ArchonOracleOptions *options,
    ArchonOracleDiagnostic *diagnostic);
```

The stage argument is required and is carried unchanged to the worker in
`ArchonOracleWorkerRequest.stage`. Generated `ArchonOracleStage` values are:

| Value | Intended boundary |
| --- | --- |
| `ARCHON_ORACLE_STAGE_INVALID` | Sentinel only; rejected by client and server |
| `ARCHON_ORACLE_STAGE_REWRITE` | Rewrite output |
| `ARCHON_ORACLE_STAGE_SCHEDULE` | Machine-IR scheduling input/output |
| `ARCHON_ORACLE_STAGE_ISEL` | Instruction-selection boundary |
| `ARCHON_ORACLE_STAGE_ISCHED` | Selected-instruction scheduling boundary |
| `ARCHON_ORACLE_STAGE_REGALLOC` | Register-allocation boundary |
| `ARCHON_ORACLE_STAGE_EMIT` | Final emission boundary |

Callers must not infer a stage from the filename. Unknown enum values are
rejected before connecting to an endpoint, including when Oracle operation
would otherwise be an unconfigured no-op. Servers likewise reject unknown
stage values in request frames before invoking the callback.

Initialize `ArchonOracleOptions` with `archon_oracle_options_init`. The runtime
does not read environment variables and does not discover or spawn a worker.
The caller configures these fields explicitly:

| `ArchonOracleOptions` field | Meaning |
| --- | --- |
| `socket_path` | AF_UNIX pathname. `NULL` or empty makes the call an inactive no-op. |
| `annotation_path` | Optional sidecar override; `NULL` derives the `.oracle.i` path. |
| `request_options` | Optional bounded opaque text delivered as `request_options` to the callback. |
| `max_artifact_bytes` | Input/output shm-region capacity; zero selects `ARCHON_ORACLE_MAX_ARTIFACT_BYTES`. |
| `max_annotation_bytes` | Annotation shm-region capacity; zero selects `ARCHON_ORACLE_MAX_ANNOTATION_BYTES`. |
| `timeout_ms` | Connect/send/receive deadline; zero selects `ARCHON_ORACLE_DEFAULT_TIMEOUT_MS`. |

Values over the generated compile-time maxima, an overlong path, or a sidecar
path equal to the artifact are rejected. No shell parsing, variable expansion,
TCP fallback, endpoint discovery, or environment override occurs.

`archon_oracle_status_string` gives a stable diagnostic spelling.
`archon_oracle_diagnostic_init` initializes an optional
`ArchonOracleDiagnostic`; failures also populate its fixed-capacity `message`
and `system_error`. Public statuses are `ARCHON_ORACLE_STATUS_OK`,
`INACTIVE`, `INVALID_ARGUMENT`, `LIMIT_EXCEEDED`, `IO_ERROR`, `TIMEOUT`,
`PROTOCOL_ERROR`, `OUT_OF_MEMORY`, `WORKER_ERROR`, `NOT_FOUND`, and
`TYPE_MISMATCH`. An unconfigured no-op returns
`ARCHON_ORACLE_STATUS_INACTIVE`, not `OK`, while leaving files untouched.

### Generated pipeline convention

`Oracle.c` itself never reads the environment. The generated command-line
stages and linked driver use `ARCHON_ORACLE_SOCKET` only as a convenience:
they copy its value into an initialized `ArchonOracleOptions.socket_path`
before calling the explicit API. An unset or empty value remains inactive.
Embedding applications set `ArchonEmbedOptions.oracle` directly; the facade
passes that same value through `ArchonPipelineOptions` and `Emitter_Options`
and never reads the convenience variable. Oracle-disabled generation omits
those fields and all Oracle headers and link dependencies.

Consumers accept only bounded, typed advice. Unknown keys are ignored; a
recognized key with the wrong type, an invalid identifier, or an out-of-range
value is rejected. Advice cannot assert semantic equivalence or make an
otherwise illegal instruction, dependency, register, or assembly operand
valid.

| Consumer | Recognized annotation keys |
| --- | --- |
| Rewriter | `rewrite.enabled` (BOOL), `rewrite.engine` (`"directed"` or `"egraph"`), `rewrite.max_iterations` (U64), `rewrite.operation.<id>.forbid` (BOOL), `rewrite.operation.<id>.priority` (I64) |
| Scheduler | `scheduler.operation.<id>.latency` (U64), `.priority` (I64), and `.depends_on.<earlier-id>` (BOOL; can only add an earlier same-block dependency) |
| ISel | `isel.pattern.<id>.deny` (BOOL) and `.cost` (bounded I64 adjustment); advice filters or re-costs only candidates that already passed semantic and operand checks |
| ISched | `isched.resource.<id>.capacity` (U64), `isched.operation.<id>.latency` (U64), and `.priority` (I64), only where the generated model marks the corresponding fact known |
| Regalloc | `regalloc.fixed_register.<value-id>` (U64 register ID), `regalloc.reserved_register.<register-id>` (BOOL), and `regalloc.spill_cost.<value-id>` (bounded U64) |
| Emitter | `emitter.dialect` (`"gas"` or `"sandstone"`), `emitter.comment` (bounded printable STRING), and `emitter.diagnostic` (bounded printable STRING); no raw assembly text is accepted |

Each sidecar's begin record must carry the consumer's stage. A file reused at
another boundary is reprocessed and receives a sidecar for that new stage;
consumers never infer the stage from a basename.

The linked driver keeps a checked in-memory program between stages. It snapshots
each materialized intermediate before an Oracle request and rejects a byte
transformation it cannot safely re-import; typed annotations still affect that
same run. File-oriented ISched, register-allocation, and emission entry points
process Oracle input before reopening it, so their normal parsers and
validators consume an atomically transformed artifact. This distinction avoids
silently ignoring a transformation or bypassing an artifact validator.

## Local protocol v1

Oracle v1 is a single-request protocol over an AF_UNIX `SOCK_STREAM`
connection:

1. The client validates the `.i` path and reads a bounded, stable snapshot.
2. It creates a unique POSIX shared-memory object with `O_CREAT|O_EXCL` and
   owner-only permissions, sizes it with checked arithmetic, and maps it.
3. A 72-byte request header carries magic `ARCHORC1`, protocol version,
   message kind, request identifier, required stage, input size, artifact and
   annotation capacities, and lengths for the shm name, full artifact path,
   and opaque request-options text. The three strings follow without NUL
   terminators.
4. Artifact bytes travel in the first `artifact_capacity` bytes of the mapping,
   not through the socket. The following `annotation_capacity` bytes are
   reserved for the response sidecar. The server opens and maps the named
   object, checks its exact total size and every declared length, and invokes
   the callback.
5. A 56-byte response header repeats the magic, version, kind, and request
   identifier and supplies a public `ArchonOracleStatus`, response flags,
   transformed-artifact size, annotation size, and diagnostic length. The
   diagnostic bytes follow. The client rejects mismatched IDs, reserved bits,
   unknown flags/statuses, out-of-range lengths, early EOF, and version or
   magic mismatches before committing a result.
6. One response terminates the connection; v1 has no pipelining,
   multiplexing, compression, descriptor passing, or network transport.

All wire integers are fixed-width little-endian values; C structure padding is
never sent. External workers should still use the generated server API rather
than duplicate framing constants. Protocol revisions must change the version
and are rejected by v1 peers.

The client owns the shared-memory name and unlinks it on success, refusal,
timeout, malformed or truncated response, allocation failure, and callback
failure. The server unmaps and closes mappings but does not unlink arbitrary
client-supplied names. The `ArchonOracleServer` owns its socket after
`archon_oracle_server_open`; `archon_oracle_server_dispose` closes it and
removes the pathname on every caller-controlled exit path.

## Generated server callback API

A worker uses the same generated source:

```c
static ArchonOracleStatus transform(
    const ArchonOracleWorkerRequest *request,
    ArchonOracleWorkerResponse *response,
    void *user,
    ArchonOracleDiagnostic *diagnostic)
{
    ArchonOracleStatus status;

    (void)user;
    (void)diagnostic;
    if (request->stage != ARCHON_ORACLE_STAGE_REWRITE)
        return ARCHON_ORACLE_STATUS_INVALID_ARGUMENT;
    if (request->artifact_size == request->artifact_capacity)
        return ARCHON_ORACLE_STATUS_LIMIT_EXCEEDED;
    request->artifact_data[request->artifact_size] = '\n';
    response->artifact_size = request->artifact_size + 1u;
    response->artifact_changed = 1;
    status = archon_oracle_response_add_u64(
        response, "cost", UINT64_C(42));
    if (status != ARCHON_ORACLE_STATUS_OK)
        return status;
    return archon_oracle_response_add_bool(response, "accepted", 1);
}

int main(void)
{
    ArchonOracleServer server;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;

    archon_oracle_server_init(&server);
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_server_open(
        &server, "/tmp/opencode/my-oracle.sock", 1u, 0u, &diagnostic);
    if (status == ARCHON_ORACLE_STATUS_OK)
        status = archon_oracle_server_serve_once(
            &server, transform, NULL, &diagnostic);
    archon_oracle_server_dispose(&server);
    return status == ARCHON_ORACLE_STATUS_OK ? 0 : 1;
}
```

`ArchonOracleWorkerRequest` exposes the validated stage, request ID, full
artifact path, opaque request options, and a mutable `artifact_data` region
with `artifact_size` and `artifact_capacity`. This permits one worker callback
to dispatch stage-specific transformations without guessing from a path.
After an in-place edit, set `ArchonOracleWorkerResponse.artifact_size` and
`artifact_changed`. The response owns a bounded annotation collection; add
unsigned, signed, Boolean, and string records with:

```c
archon_oracle_response_add_u64
archon_oracle_response_add_i64
archon_oracle_response_add_bool
archon_oracle_response_add_string
```

Keys must be nonempty, unique bounded C strings. String values are likewise
bounded and cannot contain embedded NUL bytes. The serializer escapes
non-printing bytes. The server rejects an output larger than its mapped
artifact region, duplicate keys, an invalid callback status, and annotations
whose serialized form exceeds the mapped annotation region.
`archon_oracle_server_serve_once` serves exactly one connection. A long-lived
supervisor can call it again after inspecting the status or spawn one bounded
worker per request.

## Typed annotation sidecar

The sidecar is ASCII X-macro data, not source to execute. Its schema is:

```c
ARCHON_ORACLE_BEGIN(1, 1)
ARCHON_ORACLE_U64("cost", UINT64_C(42))
ARCHON_ORACLE_I64("delta", INT64_C(-7))
ARCHON_ORACLE_BOOL("accepted", 1)
ARCHON_ORACLE_STRING("phase", "peephole")
ARCHON_ORACLE_END()
```

The begin record contains the protocol version and numeric
`ArchonOracleStage`; `1` is `ARCHON_ORACLE_STAGE_REWRITE`. Body order is
callback insertion order and emission is deterministic. Strings use strict C
string-literal escaping so a sidecar can be included as an X-macro fragment,
but the runtime parser does **not** run a C preprocessor. It accepts only the
six records above, their exact arities and integer token forms, bounded
whitespace, and one complete begin/end envelope. Unknown or duplicate records
and keys, bad escapes, invalid Booleans, integer overflow, invalid versions or
stages, trailing tokens, truncation, and files larger than
`ARCHON_ORACLE_MAX_ANNOTATION_BYTES` are rejected.

Runtime consumers parse and query sidecars without preprocessing:

```c
ArchonOracleAnnotations annotations;
ArchonOracleDiagnostic diagnostic;
uint64_t cost;
const char *phase;

archon_oracle_annotations_init(&annotations);
archon_oracle_diagnostic_init(&diagnostic);
if (archon_oracle_annotations_load(
        "Program.oracle.i",
        &annotations, &diagnostic) == ARCHON_ORACLE_STATUS_OK) {
    if (annotations.stage == ARCHON_ORACLE_STAGE_REWRITE &&
            archon_oracle_annotations_get_u64(
                &annotations, "cost", &cost) ==
                ARCHON_ORACLE_STATUS_OK) {
        /* use cost */
    }
    if (archon_oracle_annotations_get_string(
            &annotations, "phase", &phase) ==
            ARCHON_ORACLE_STATUS_OK) {
        /* phase is owned by annotations */
    }
}
archon_oracle_annotations_dispose(&annotations);
```

`archon_oracle_annotations_load` is the canonical file loader. The validated
records are exposed through `annotations.entries` and `annotations.count`;
`annotations.protocol_version` and `annotations.stage` preserve the begin
record. The typed `get_u64`, `get_i64`, `get_bool`, and `get_string` functions
return `ARCHON_ORACLE_STATUS_NOT_FOUND` for an absent key and
`ARCHON_ORACLE_STATUS_TYPE_MISMATCH` for a wrong kind. Values are never
coerced. `archon_oracle_annotations_dispose` releases all owned keys and
strings.

## Commit and failure behavior

The original artifact is not opened for in-place mutation. Only after the
entire response and annotation stream pass validation does the client:

1. create same-directory temporary files without following symlinks;
2. write all bytes, check close/flush errors, and preserve the artifact's
   intended permission mode;
3. atomically rename the sidecar first and then any transformed artifact; and
4. remove every temporary path.

Each rename is atomic to other filesystem users. Two independent directory
entries cannot form one portable filesystem transaction, so the artifact and
sidecar pair is not promised to appear simultaneously. Callers that require a
cross-file snapshot must add their own directory-level generation/manifest
protocol.

On a connection, protocol, validation, size, timeout, or callback failure, the
input artifact is unchanged and no new sidecar is installed. Existing output
files are never truncated before a complete replacement exists. Socket,
mapping, descriptor, shared-memory name, and temporary-file cleanup is required
on all exits.

## Trust and threat boundaries

The transport is hardened against malformed local messages; it is not a
sandbox or an authentication protocol.

- The configured worker is **semantically trusted**. It may replace compiler
  artifacts and thereby affect generated code. Run only a worker selected by
  the build owner.
- AF_UNIX and POSIX shared memory provide local IPC, not peer identity,
  confidentiality, integrity, replay protection, or encryption. Put the
  socket in a private directory such as a mode-0700 directory below
  `/tmp/opencode`; do not use a shared writable pathname.
- The client and server treat all lengths, offsets, names, transformed bytes,
  and annotations as untrusted and apply limits before allocation or mapping.
  Limits bound individual requests, not aggregate CPU consumption by a
  malicious peer.
- Sidecars are data. Prefer the checked parser. Including a sidecar through a C
  preprocessor is appropriate only when the producer is trusted and all six
  macros are defined by the consumer.
- Atomic rename prevents readers from observing partial bytes; it does not
  prevent another process from racing to replace the path before or after the
  operation. Build directories must have trustworthy ownership and
  permissions.
- The generated callback executes native worker code in the worker process.
  The API validates its output but cannot contain arbitrary callback behavior.
