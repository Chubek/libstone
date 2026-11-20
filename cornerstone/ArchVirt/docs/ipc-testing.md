# Testing an OS through IPC

ArchVirt exposes a versioned JSON protocol on a local Unix stream socket. It
supports all DomIOS machine profiles (`x86_64`, `aarch64`, and `riscv64`) and custom ELF64
kernels using the [boot ABI](../include/archvirt_boot.h). The supported CPU and
platform scope is described in the [README](../README.md#machine-and-execution-model).
It also supports the native x86 [B1 PC BIOS](bios.md) disk-boot path.

## Python test fixture

`tools/archvirt_ipc.py` provides a dependency-free client and a context manager
that starts a server, waits for readiness, and cleans up its process/socket.
Add `tools` to your Python import path:

```sh
PYTHONPATH=tools python3 my_os_test.py
```

```python
from archvirt_ipc import TestVM

with TestVM("build/archvirt") as vm:
    vm.boot(machine="aarch64", cmdline="test=1")
    vm.expect("domix# ")
    vm.send("uname\n")
    vm.expect("Domix aarch64\n")
    vm.send("echo hello > /note\ncat /note\n")
    vm.expect("domix# cat /note\nhello\n")
    vm.send("exit\n")
    result = vm.wait()
    assert result["state"] == "halted"
    assert result["result"]["code"] == 0
```

For your own kernel, use absolute host paths (relative paths resolve in the
server's working directory):

```python
with TestVM("build/archvirt") as vm:
    vm.boot(machine="x86_64", kernel="/path/to/kernel.elf",
            image="/path/to/initrd.img", cmdline="selftest=1", paused=True,
            max_instructions=5_000_000)
    regs = vm.registers()
    assert regs["rdi"] == 0x10000  # boot-info pointer; x0 on AArch64, x10 on RV64
    print(vm.read_memory(regs["pc"], 16).hex())
    vm.step(1)
    vm.resume()
    vm.expect(b"SELFTEST PASS\n", timeout=10)
    result = vm.wait(timeout=10)
    assert result["state"] == "halted" and result["result"]["code"] == 0
```

Supplying `kernel` disables bundled Domix by default. `image` then contains
arbitrary guest data; with `domix=true`, it must be a packed Domix image.
DomIOS firmware remains enabled unless `domios=false` is supplied. ELF segments
must be identity mapped within `0x200000..0x600000`; RAM, devices and entry
registers follow the selected profile. Software stepping is at guest instruction
granularity. A paused boot stops before the first kernel instruction, after
firmware preparation and loading.

For BIOS, request `boot_mode="bios"`, `backend="kvm"`, and `disk="/path/to/disk.img"`
instead of a direct kernel. A paused boot now stops at the reset vector, before
firmware POST or disk reads. `firmware` optionally names a B1-layout ROM.
Direct `kernel`, `image`, `cmdline`, instruction limits and software execution
are rejected for BIOS. Reset reloads the host disk and discards volatile writes.

`expect()` accepts bytes or UTF-8 text, consumes output through the marker, and
retains trailing bytes for the next call. It reports timeouts, premature guest
termination, capture overflow, and generation changes instead of silently
passing. `read_console()` returns captured bytes; `send()` accepts bytes or text.
`wait()` polls for completion and returns status, including failures; tests must
assert the result code. Per-request socket timeout defaults to 10 seconds and is
configurable with `TestVM(..., timeout=30)` or `Client(path, timeout=30)`.

## Standalone server

```sh
build/archvirt --test-server /path/to/test.sock
python3 tools/archvirt_ipc.py /path/to/test.sock boot '{"machine":"aarch64"}'
python3 tools/archvirt_ipc.py /path/to/test.sock console-write '{"text":"uname\n"}'
python3 tools/archvirt_ipc.py /path/to/test.sock console-read
python3 tools/archvirt_ipc.py /path/to/test.sock quit
```

The server starts empty and remains available after halt, stop, or a failed boot.
It is headless, captures guest serial exclusively through IPC, and defaults to
the software backend. Other CLI options establish defaults for each `boot`.
Use `--backend kvm` or `boot`'s `backend` field for native x86 execution. KVM
supports pause/resume and inspection, but no single-step or instruction counts.
`max_instructions` requires software execution (`auto` selects software when a
limit is set).

`--control PATH` attaches the same inspection/console commands to an ordinary
CLI run; it boots immediately and exits with the guest. `boot` and `reset`
require `--test-server`. Terminal/GUI input and IPC input share the serial
device in ordinary CLI mode. The socket is mode `0600`, supports up to 32
connected clients, and refuses to replace an existing path. Use a unique path
per runner. Normal shutdown removes the socket.

## Wire protocol, version 1

Open an `AF_UNIX`/`SOCK_STREAM` connection, send one UTF-8 JSON object followed by
newline, and read one newline-terminated JSON response. The server closes that
connection after replying; open another for the next request. Fragmented writes
and large responses are supported. For legacy clients, a complete JSON object
without a newline or a write-side shutdown also terminates the request.

```json
{"id":42,"execute":"read-memory","arguments":{"address":65536,"size":8}}
{"id":42,"return":{"address":65536,"data_hex":"415256424f4f5431"}}
```

`id` is optional and echoed on parsed requests, including errors. `command` is
accepted as an alias for `execute`. Arguments default to `{}`; unknown argument
names, incorrect types, and unsupported commands return errors. Integers are
unsigned JSON numbers; clients must preserve full 64-bit values. An error has
the shape `{"error":{"code":"invalid-argument","message":"..."}}`.
VM failures use `code: "vm-error"` and include the numeric `vm_code`.
Transport/parser errors use `request-too-large` or `invalid-request`; unknown
commands use `unsupported-command`. Requests are limited to 256 KiB. Incomplete
requests and stalled response readers are disconnected after five seconds.
Commands execute serially; idle connections do not block other clients.

| Command | Arguments | Result |
| --- | --- | --- |
| `query-capabilities` | none | Protocol version, supported machines/commands, managed mode, limits |
| `query-status` (`status`) | none | State, machine, backend, instruction count, generation, result |
| `boot` | See below | Replaces the VM and returns status |
| `reset` | `paused` (default false) | Recreates the last successfully booted configuration from its images |
| `pause` / `resume` | none | Waits for the transition and returns status |
| `step` | `count` (1–10000, default 1) | Executes while paused; returns status, or an execution error |
| `stop` | none | Stops and joins the guest; returns status, retaining memory and output |
| `quit` | none | Stops the guest, replies, and exits the server |
| `console-write` | Exactly one of `text` or `data_hex` | `accepted`: bytes queued for serial input |
| `console-read` | `offset` (default 0), `limit` (1–65536, default 65536), optional `generation` | See capture cursor below |
| `query-screen` | none | `columns`, `rows`, `format: "vga-text"`, `data_hex`, `generation` |
| `query-registers` | none | `machine` and `registers` map |
| `read-memory` | `address`, `size` (1–65536) | `address`, `data_hex` |
| `write-memory` | `address`, `data_hex` (1–65536 decoded bytes) | `written` |

`boot` accepts `machine`, `backend`, `archml`, `kernel`, `image`, `firmware`,
`cmdline`, `memory_mb` (40–1024), `max_instructions` (0 means unlimited),
`domix`, `domios`, `paused`, `boot_mode` (`direct` or `bios`), and `disk`.
Unspecified fields come from CLI defaults,
not the previous boot. A boot attempt stops the old VM and clears RAM, queued
input, output, and screen. Initialization failure leaves an empty VM with an
error result. `reset` retains the last successful configuration even after a
failed boot, reloads files, and discards all guest filesystem overlay changes.

States are `empty`, `ready`, `running`, `paused`, `halted`, `stopped`, or `failed`.
`active` includes paused VMs; `running` means the CPU is executing. `instructions`
counts successfully executed software instructions, including HLT; consult
`instruction_count_supported` before using it. `result` contains `code` (0 is
success), `message`, and `complete`. HLT completes execution successfully;
unsupported instructions and instruction-budget exhaustion produce failures.
These are VM results, not process exit codes from a guest OS. A successful
`boot` response means initialization succeeded; query status for later faults.
The test-server process exits successfully on `quit` even after a guest failure.

Register/memory reads require a paused or completed VM; writes require a paused
VM. Addresses are guest physical memory addresses, with bounds checking; BIOS
ROM is readable but rejects writes. Native x86 register maps additionally
expose `cs`, `cs_base`, `cr0`, `cr3`, and `efer`. Register
maps expose common `pc` and `sp` aliases, plus x86 GPRs, `rip`, `rsp`, `rflags`,
or AArch64 `x0`–`x30`, `pc`, `sp`, `nzcv`. Screen data is 80×25 character/attribute
pairs (4000 bytes), empty before the first snapshot. Snapshots update during
execution and at pause, step, and completion.

### Console capture cursors

Serial output is byte-exact, encoded as hex, with a 1 MiB ring retained per boot.
Reads do not consume the shared ring, allowing independent readers. Responses
contain `generation`, `begin` (oldest retained offset), `end` (total bytes written),
`next` (offset for the next read), `truncated`, and `data_hex`. Start at offset 0,
then pass back `next` and `generation`. Offsets beyond `end` are rejected; offsets
before `begin` return available bytes with `truncated=true`. Every boot attempt
increments `generation`; an old supplied generation is rejected. The Python
client tracks these fields and resets its cursor on `boot()`/`reset()`.

Input has a 64 KiB queue. A request that would overflow it fails without partial
insertion. Input can be queued while paused. The polled serial ABI uses zero for
no input, so a guest cannot distinguish an injected NUL from an empty queue.
Console capture observes guest serial bytes; firmware host diagnostics and GUI
status text are separate. There is no guest agent or file-transfer requirement.

RISC-V register maps expose `x0`..`x31`, `pc`, and `sp` (alias of `x2`).
Its backend is software only; see [the execution contract](riscv64.md).

The CTest `ipc_x86_64`, `ipc_aarch64`, `ipc_riscv64`, and optional `ipc_native_kvm` tests exercise
actual boot, shell/filesystem commands, reset, custom ELF images, binary output,
pause/resume, inspection, interruption, and errors through this interface.
