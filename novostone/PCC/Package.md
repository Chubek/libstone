# Signed PCC package v1

Build Novostone with `-DNOVOSTONE_PCC_SIGNED_PACKAGES=ON` (OpenSSL 3 Crypto)
and configure `-DNOVOSTONE_PCC_REQUIRE_SATIE=ON` for verification. The
installed CMake target is `Novostone::PCCPackage`.

`pcc-pack` produces a deterministic **source-level** package. `pcc-verify`
and `pcc-serve` authenticate and re-prove it, rather than treating a signature
as a proof. The verifier requires a locally configured Ed25519 public key and
the compiled Satie SMT-LIB2 conduit. There is no package-supplied key, solver
configuration, script, proof status, path, native code, or register-allocation
attachment. The verifier does not execute the IR. Success covers the signed
source and optimized IR under Novostone's i32 semantics, not arbitrary native
machine code or an external program's identity.

## Local commands

```sh
umask 077
openssl genpkey -algorithm ED25519 -out signer.pem
openssl pkey -in signer.pem -pubout -out signer.pub.pem
novostone pcc-pack novostone/examples/pcc.nir --sign-key signer.pem > signed.pcc
novostone pcc-verify signed.pcc --trust-key signer.pub.pem
```

Use unencrypted PKCS#8 PEM for the private key (keep it offline or on
the producer, never on the verifier); provision the SubjectPublicKeyInfo public
key independently. An OpenSSH key file is not a PEM signing key for this
protocol. Key rotation requires coordinated replacement of the configured
trust key and re-signing; wire v1 does not provide key discovery, expiry, or
revocation. `pcc-pack` has optional `--transforms NAME[,NAME...]` and
`--max-rewrites N` arguments after the key. A successful pack means it signed
an artifact, not that the artifact can be verified; verify before release.

Existing `novostone pcc ... --frontend smtlib2` output beginning `pcc-package 1`
is a diagnostic report, **not** v1 wire bytes. It is deliberately rejected
by the wire reader. Migrate by repacking the original `.nir` source with
the intended transform settings and signer; do not turn a report or its
packaged statuses into a trust decision. Decoder and verifier must be updated
together for incompatible IR/replay semantics. If a locally upgraded optimizer
selects a different candidate, the old package fails closed; retain an
appropriate verifier or regenerate and re-sign after reviewing the change.

## Authenticated remote verification with OpenSSH

The service is one request per no-PTY SSH session, reading a single binary
package up to EOF. Configure a dedicated unprivileged `pccverify` account
with a shell capable of running the administrator's fixed command; disable
ordinary login for that account using an `sshd_config` match block (not a
login shell that also prevents forced commands). Example on a Unix host:

```text
Match User pccverify
    AuthenticationMethods publickey
    PasswordAuthentication no
    KbdInteractiveAuthentication no
    PermitUserEnvironment no
    PermitUserRC no
    PermitTTY no
    DisableForwarding yes
    ForceCommand /usr/bin/timeout --kill-after=2s 20s /opt/novostone/bin/novostone pcc-serve --trust-key /etc/novostone/signer.pub.pem
```

Use `restrict` in the account's administrator-managed `authorized_keys` entry
for the SSH client's public key. The verifier binary, trust key, SSH
configuration and login shell must not be writable by the service account.
Don't accept environment overrides on the service account. Run it with no
secrets, no write access to trusted files, and administrator-enforced
process, memory, CPU and concurrency limits. Source-level work limits are
not a system sandbox or a solver deadline. The outer timeout must include
read time as a peer can otherwise withhold EOF. There is no daemon or
unencrypted network mode in Novostone.

Provision an independently obtained SSH **host key** in a dedicated
`known_hosts` file (don't blindly enroll keys returned by the network).
The SSH user key authorizes access to the service; the separately configured
package signing key authorizes inputs to replay. A client can use ordinary
OpenSSH; configure the endpoint and all local credential paths independently
of the package:

```sh
timeout 25s ssh -F /dev/null -T -i ./pcc-client \
  -o BatchMode=yes -o IdentitiesOnly=yes -o IdentityAgent=none \
  -o StrictHostKeyChecking=yes -o UserKnownHostsFile=./pcc-known_hosts \
  -o GlobalKnownHostsFile=/dev/null -o UpdateHostKeys=no -o ConnectTimeout=5 \
  pccverify@verifier.example.com < signed.pcc
```

Only exit status **0** together with exactly `PCC-VERIFY/1 OK\n` on stdout
means acceptance. `PCC-VERIFY/1 REJECT\n` and exit 1 mean verification failed.
Other responses, stale logs, partial responses, disconnects, SSH peer
authentication failures and timeouts are not positive verification. The
protocol attests to the request on that authenticated SSH channel only; it
does not issue a portable receipt or prevent retrial of a previously signed
package. Do not pipe output in a way that loses the `ssh` status.

## Wire layout and limits

All integer fields are unsigned big-endian; widths are octets. There is
exactly one binary encoding, with no padding or trailing bytes.

| Field | Encoding |
|---|---|
| Magic | 8 ASCII octets `NSPCCPKG` |
| Wire version, artifact schema | two u32 fields, both `1` |
| Payload length | u32, not including the header or signature |
| Payload | constraints, source function, then candidate function |
| Signature | 64 octets of pure Ed25519 over the literal header and payload |

Constraints are one u32 transform mask, bits 0–8 in order:
constant folding, local CSE, algebraic simplification, copy propagation,
dead code elimination, branch folding, phi simplification, load forwarding,
conditional constant propagation. High bits are invalid. The mask is followed
by eight u32 work caps: maximum arity, blocks, instructions, references,
rewrites, query work, query facts, and analysis work.

Each function has u32 arity, u64 entry ID, u32 block count. Each block has u64
ID and u32 instruction count, then its instructions in execution order.
Blocks must appear in strictly ascending ID order. Each instruction has u64
ID, u32 opcode, u32 immediate, u32 operand count and that many u64 IDs, then
u32 target count and that many u64 IDs. Opcode numbers 0–14 mean `param`,
`const`, `copy`, `add`, `sub`, `mul`, `eq`, `ult`, `select`, `load`, `store`,
`jump`, `branch`, `return`, `phi`, respectively. IDs must be in
`[1, INT64_MAX-1]`; arity is allowed to be zero. The core verifier supplies
the remaining IR invariants.

The verifier caps wire bytes at 64 KiB and PEM files at 16 KiB. Before
touching the proof engine it bounds and authenticates the entire frame;
internal counts are checked against remaining bytes and local policy before
allocation. The default work caps, in wire order, are
`64, 32, 256, 1024, 16, 1000000, 50000, 100000`, with structural counts
per function and optimizer caps per run. Applications can tighten limits
using `PackagePolicy` but cannot raise them. An explicitly bounded rewrite
prefix is permitted, while failed analysis or exhausted optimizer work
cannot authorize a package.

The verifier rebuilds the artifact and compares the candidate bytes. A second
bounded replay of the accepted prefix checks every rewrite and intermediate
state and regenerates the corresponding SMT-LIB2. A cumulative 512 KiB cap
limits generated SMT bytes sent to the local solver; generation itself is
bounded by the core emitter's separate circuit and 64 MiB text limits.
Every obligation must
return `UnsatProved` with valid syntax; no SAT, unknown, absent solver or
unsupported rule is accepted. An unchanged candidate requires no obligations,
but still requires a solver-enabled verifier build. The trusted computing
base includes Novostone's replay, encoding and Satie (this format does not
check a separate solver proof certificate), OpenSSL and, remotely, OpenSSH
and the verifier host.

## Transport test

On Unix with Python 3, OpenSSL 3.x command-line tools and OpenSSH client and
server, from the repository root run:

```sh
python3 novostone/tests/pcc-package-ssh.py /path/to/build/novostone \
  novostone/examples/pcc.nir --sshd /usr/bin/sshd
```

Use the locally installed `sshd` path. The test starts a temporary loopback
server with fresh host/client keys and a forced command; it does not modify
system SSH configuration. It tests good and bad signing keys, tampering,
framing, independently pinned host keys, and SSH client authentication.
`--expect-no-solver` makes the same test assert rejection of an otherwise
valid signed package with a solver-disabled build.
